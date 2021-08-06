// Copyright © 2021 Intel Corporation
//
// SPDX-License-Identifier: Apache-2.0
//

#[cfg(test)]
#[macro_use]
extern crate lazy_static;
extern crate regex;

#[cfg(test)]
mod tests {
    use regex::Regex;
    use simple_xml_serialize::XMLElement;
    use std::io::{self, Write};
    use std::process::{Child, Command, Stdio};
    use std::sync::Mutex;
    use std::thread;
    use std::{ffi::OsStr, path::PathBuf};
    use test_infra::*;
    use uuid::Uuid;
    use vmm_sys_util::tempdir::TempDir;

    const FOCAL_IMAGE_NAME: &str = "focal-server-cloudimg-amd64.raw";

    pub const DEFAULT_TCP_LISTENER_PORT: u16 = 8000;
    const DEFAULT_RAM_SIZE: u64 = 1 << 30;

    lazy_static! {
        static ref NEXT_VM_ID: Mutex<u8> = Mutex::new(1);
    }

    #[derive(Debug)]
    enum Error {
        Parsing(std::num::ParseIntError),
        SshCommand(SshCommandError),
        WaitForBoot(WaitForBootError),
    }

    impl From<SshCommandError> for Error {
        fn from(e: SshCommandError) -> Self {
            Self::SshCommand(e)
        }
    }

    struct VcpuConfig {
        boot: u8,
        max: u8,
    }

    impl Default for VcpuConfig {
        fn default() -> Self {
            VcpuConfig { boot: 1, max: 1 }
        }
    }

    #[derive(PartialEq)]
    enum KernelType {
        Direct,
        OvmfFw,
        RustFw,
    }

    impl KernelType {
        fn path(&self) -> PathBuf {
            #[cfg(target_arch = "aarch64")]
            assert!(self == KernelType::Direct);

            let mut kernel_path = dirs::home_dir().unwrap();
            kernel_path.push("workloads");

            match self {
                KernelType::Direct => {
                    #[cfg(target_arch = "aarch64")]
                    kernel_path.push("Image");
                    #[cfg(target_arch = "x86_64")]
                    kernel_path.push("vmlinux");
                }
                KernelType::OvmfFw => {
                    kernel_path.push("OVMF-4b47d0c6c8.fd");
                }
                KernelType::RustFw => {
                    kernel_path.push("hypervisor-fw");
                }
            }

            kernel_path
        }
    }

    struct Guest<'a> {
        tmp_dir: TempDir,
        vm_name: String,
        uuid: String,
        kernel: KernelType,
        network: GuestNetworkConfig,
        disk_config: &'a dyn DiskConfig,
    }

    impl<'a> std::panic::RefUnwindSafe for Guest<'a> {}

    impl<'a> Guest<'a> {
        fn create_domain(&self, vcpus: VcpuConfig, memory_size: u64) -> PathBuf {
            let mut xml_os = XMLElement::new("os")
                .element(XMLElement::new("type").text("hvm"))
                .element(XMLElement::new("kernel").text(self.kernel.path().to_str().unwrap()));
            if self.kernel == KernelType::Direct {
                xml_os.add_element(XMLElement::new("cmdline").text("root=/dev/vda1 rw"));
            }

            let xml_domain = XMLElement::new("domain")
                .attr("type", "ch")
                .element(XMLElement::new("name").text(self.vm_name.clone()))
                .element(XMLElement::new("uuid").text(self.uuid.clone()))
                .element(XMLElement::new("genid").text("43dc0cf8-809b-4adb-9bea-a9abb5f3d90e"))
                .element(XMLElement::new("title").text(format!("Test VM {}", self.vm_name.clone())))
                .element(
                    XMLElement::new("description")
                        .text(format!("Test VM {}", self.vm_name.clone())),
                )
                .element(xml_os)
                .element(
                    XMLElement::new("vcpu")
                        .attr("current", vcpus.boot)
                        .text(vcpus.max),
                )
                .element(
                    XMLElement::new("memory")
                        .attr("unit", "b")
                        .text(memory_size),
                )
                .element(
                    XMLElement::new("devices")
                        .element(
                            XMLElement::new("disk")
                                .attr("type", "file")
                                .element(XMLElement::new("source").attr(
                                    "file",
                                    self.disk_config.disk(DiskType::OperatingSystem).unwrap(),
                                ))
                                .element(
                                    XMLElement::new("target")
                                        .attr("dev", "vda")
                                        .attr("bus", "virtio"),
                                ),
                        )
                        .element(
                            XMLElement::new("disk")
                                .attr("type", "file")
                                .element(XMLElement::new("source").attr(
                                    "file",
                                    self.disk_config.disk(DiskType::CloudInit).unwrap(),
                                ))
                                .element(
                                    XMLElement::new("target")
                                        .attr("dev", "vdb")
                                        .attr("bus", "virtio"),
                                ),
                        )
                        .element(
                            XMLElement::new("console").attr("type", "pty").element(
                                XMLElement::new("target")
                                    .attr("type", "virtio")
                                    .attr("port", "0"),
                            ),
                        )
                        .element(
                            XMLElement::new("interface")
                                .attr("type", "ethernet")
                                .element(
                                    XMLElement::new("mac")
                                        .attr("address", self.network.guest_mac.clone()),
                                )
                                .element(XMLElement::new("model").attr("type", "virtio"))
                                .element(
                                    XMLElement::new("source").element(
                                        XMLElement::new("ip")
                                            .attr("address", self.network.host_ip.clone())
                                            .attr("prefix", "24"),
                                    ),
                                ),
                        ),
                );

            let domain = xml_domain.to_string();

            eprintln!("{}\n", domain);

            let mut domain_path = self.tmp_dir.as_path().to_path_buf();
            domain_path.push("domain.xml");

            let mut f = std::fs::File::create(&domain_path).unwrap();
            f.write_all(&domain.as_bytes()).unwrap();

            domain_path
        }

        fn new_from_ip_range(
            disk_config: &'a mut dyn DiskConfig,
            class: &str,
            id: u8,
            kernel: KernelType,
        ) -> Self {
            let tmp_dir = TempDir::new_with_prefix("/tmp/ch").unwrap();

            let network = GuestNetworkConfig {
                guest_ip: format!("{}.{}.2", class, id),
                l2_guest_ip1: format!("{}.{}.3", class, id),
                l2_guest_ip2: format!("{}.{}.4", class, id),
                l2_guest_ip3: format!("{}.{}.5", class, id),
                host_ip: format!("{}.{}.1", class, id),
                guest_mac: format!("12:34:56:78:90:{:02x}", id),
                l2_guest_mac1: format!("de:ad:be:ef:12:{:02x}", id),
                l2_guest_mac2: format!("de:ad:be:ef:34:{:02x}", id),
                l2_guest_mac3: format!("de:ad:be:ef:56:{:02x}", id),
                tcp_listener_port: DEFAULT_TCP_LISTENER_PORT + id as u16,
            };

            disk_config.prepare_files(&tmp_dir, &network);

            let vm_name = format!("vm-{}", id);
            Guest {
                tmp_dir,
                disk_config,
                kernel,
                network,
                vm_name,
                uuid: Uuid::new_v4().to_hyphenated().to_string(),
            }
        }

        fn new(disk_config: &'a mut dyn DiskConfig, kernel: KernelType) -> Self {
            let mut guard = NEXT_VM_ID.lock().unwrap();
            let id = *guard;
            *guard = id + 1;

            Self::new_from_ip_range(disk_config, "192.168", id, kernel)
        }

        fn wait_vm_boot(&self, custom_timeout: Option<i32>) -> Result<(), Error> {
            // Focal image requires more than default 80s to boot, that's why
            // we set the default to 120s.
            self.network
                .wait_vm_boot(custom_timeout.or(Some(120)))
                .map_err(Error::WaitForBoot)
        }

        fn ssh_command(&self, command: &str) -> Result<String, SshCommandError> {
            ssh_command_ip(
                command,
                &self.network.guest_ip,
                DEFAULT_SSH_RETRIES,
                DEFAULT_SSH_TIMEOUT,
            )
        }

        fn get_cpu_count(&self) -> Result<u8, Error> {
            self.ssh_command("grep -c processor /proc/cpuinfo")?
                .trim()
                .parse()
                .map_err(Error::Parsing)
        }

        fn get_total_memory(&self) -> Result<u32, Error> {
            self.ssh_command("grep MemTotal /proc/meminfo | grep -o \"[0-9]*\"")?
                .trim()
                .parse()
                .map_err(Error::Parsing)
        }
    }

    fn spawn_libvirtd() -> io::Result<Child> {
        Command::new("libvirtd")
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
    }

    fn spawn_virsh<I, S>(args: I) -> io::Result<Child>
    where
        I: IntoIterator<Item = S>,
        S: AsRef<OsStr>,
    {
        Command::new("virsh")
            .args(&["-c", "ch:///system"])
            .args(args)
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
    }

    fn cleanup_libvirt_state() {
        let _ = std::fs::remove_dir_all("/etc/libvirt/ch");
        let _ = std::fs::remove_dir_all("/var/lib/libvirt");
        let _ = std::fs::remove_file("/var/run/libvirtd.pid");
        let _ = std::fs::remove_dir_all("/var/run/libvirt");
    }

    fn test_create_vm(kernel: KernelType) {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, kernel);

        let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);

        let r = std::panic::catch_unwind(|| {
            let output = spawn_virsh(&["create", domain_path.to_str().unwrap()])
                .unwrap()
                .wait_with_output()
                .unwrap();

            eprintln!(
                "create stdout\n\n{}\n\ncreate stderr\n\n{}",
                std::str::from_utf8(&output.stdout).unwrap(),
                std::str::from_utf8(&output.stderr).unwrap()
            );

            assert!(std::str::from_utf8(&output.stdout)
                .unwrap()
                .trim()
                .starts_with(&format!("Domain {} created", guest.vm_name)));

            guest.wait_vm_boot(None).unwrap();
        });

        let destroy_output = spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait_with_output()
            .unwrap();

        eprintln!(
            "destroy stdout\n\n{}\n\ndestroy stderr\n\n{}",
            std::str::from_utf8(&destroy_output.stdout).unwrap(),
            std::str::from_utf8(&destroy_output.stderr).unwrap()
        );

        assert!(std::str::from_utf8(&destroy_output.stdout)
            .unwrap()
            .trim()
            .starts_with(&format!("Domain {} destroyed", guest.vm_name)));

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(r.is_ok());
    }

    #[test]
    fn test_defines() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);
        let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);
        let output = spawn_virsh(&["define", domain_path.to_str().unwrap()])
            .unwrap()
            .wait_with_output()
            .unwrap();

        libvirtd.kill().unwrap();
        // libvirtd got SIGKILL, cleanup /var/run manually
        // to avoid getting non-persistent state leftovers
        let _ = std::fs::remove_dir_all("/var/lib/libvirt");
        let _ = std::fs::remove_file("/var/run/libvirtd.pid");
        let _ = std::fs::remove_dir_all("/var/run/libvirt");

        // verify persistent state exists
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));
        let list_output = spawn_virsh(&["list", "--all"])
            .unwrap()
            .wait_with_output()
            .unwrap();

        let undefine_output = spawn_virsh(&["undefine", &guest.vm_name])
            .unwrap()
            .wait_with_output()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(std::str::from_utf8(&output.stdout)
            .unwrap()
            .trim()
            .starts_with(&format!("Domain {} defined", guest.vm_name)));

        let re = Regex::new(&format!(r"\s+-\s+{}\s+shut off", guest.vm_name)).unwrap();
        assert!(re.is_match(std::str::from_utf8(&list_output.stdout).unwrap().trim()));

        assert!(std::str::from_utf8(&undefine_output.stdout)
            .unwrap()
            .trim()
            .starts_with(&format!("Domain {} has been undefined", guest.vm_name)));
    }

    #[test]
    fn test_direct_kernel_boot() {
        test_create_vm(KernelType::Direct)
    }

    #[test]
    fn test_libvirt_restart() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);
        let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);

        spawn_virsh(&["create", domain_path.to_str().unwrap()])
            .unwrap()
            .wait()
            .unwrap();

        guest.wait_vm_boot(None).unwrap();
        libvirtd.kill().unwrap();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let destroy_output = spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait_with_output()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(std::str::from_utf8(&destroy_output.stdout)
            .unwrap()
            .trim()
            .starts_with(&format!("Domain {} destroyed", guest.vm_name)));
    }

    #[test]
    fn test_huge_memory() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);

        let domain_path = guest.create_domain(VcpuConfig::default(), 128 << 30);

        let r = std::panic::catch_unwind(|| {
            spawn_virsh(&["create", domain_path.to_str().unwrap()])
                .unwrap()
                .wait()
                .unwrap();

            guest.wait_vm_boot(None).unwrap();

            assert!(guest.get_total_memory().unwrap_or_default() > 128_000_000);
        });

        spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(r.is_ok());
    }

    #[test]
    fn test_multi_cpu() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);

        let domain_path = guest.create_domain(VcpuConfig { boot: 2, max: 4 }, DEFAULT_RAM_SIZE);

        let r = std::panic::catch_unwind(|| {
            spawn_virsh(&["create", domain_path.to_str().unwrap()])
                .unwrap()
                .wait()
                .unwrap();

            guest.wait_vm_boot(None).unwrap();

            // Check the number of vCPUs matches 'boot' parameter.
            assert_eq!(guest.get_cpu_count().unwrap_or_default(), 2);

            #[cfg(target_arch = "x86_64")]
            assert_eq!(
                guest
                    .ssh_command(r#"dmesg | grep "smpboot: Allowing" | sed "s/\[\ *[0-9.]*\] //""#)
                    .unwrap()
                    .trim(),
                "smpboot: Allowing 4 CPUs, 2 hotplug CPUs"
            );
            #[cfg(target_arch = "aarch64")]
            assert_eq!(
                guest
                    .ssh_command(r#"dmesg | grep "smp: Brought up" | sed "s/\[\ *[0-9.]*\] //""#)
                    .unwrap()
                    .trim(),
                "smp: Brought up 1 node, 2 CPUs"
            );

            // Hotplug 2 vCPUs
            spawn_virsh(&["setvcpus", &guest.vm_name, "4"])
                .unwrap()
                .wait()
                .unwrap();

            // Online them from the guest
            guest
                .ssh_command("echo 1 | sudo tee /sys/bus/cpu/devices/cpu2/online")
                .unwrap();
            guest
                .ssh_command("echo 1 | sudo tee /sys/bus/cpu/devices/cpu3/online")
                .unwrap();

            // Check the number of vCPUs has been increased.
            assert_eq!(guest.get_cpu_count().unwrap_or_default(), 4);

            // Unplug 3 vCPUs
            spawn_virsh(&["setvcpus", &guest.vm_name, "1"])
                .unwrap()
                .wait()
                .unwrap();

            // Check the number of vCPUs has been reduced.
            assert_eq!(guest.get_cpu_count().unwrap_or_default(), 1);
        });

        spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(r.is_ok());
    }

    #[test]
    fn test_ovmf_fw_boot() {
        test_create_vm(KernelType::OvmfFw)
    }

    #[test]
    fn test_rust_fw_boot() {
        test_create_vm(KernelType::RustFw)
    }

    #[test]
    fn test_track_vm_killed_state() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);

        let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);

        let r = std::panic::catch_unwind(|| {
            spawn_virsh(&["create", domain_path.to_str().unwrap()])
                .unwrap()
                .wait_with_output()
                .unwrap();

            guest.wait_vm_boot(None).unwrap();

            let pid = std::fs::read_to_string(format!("/var/run/libvirt/ch/{}.pid", guest.vm_name))
                .unwrap()
                .parse::<libc::pid_t>()
                .unwrap();
            unsafe {
                libc::kill(pid, libc::SIGKILL);
            }
            thread::sleep(std::time::Duration::new(2, 0));

            let list_output = spawn_virsh(&["list", "--all"])
                .unwrap()
                .wait_with_output()
                .unwrap();

            let re = Regex::new(&format!(r"\s+-\s+{}\s+shut off", guest.vm_name)).unwrap();
            assert!(re.is_match(std::str::from_utf8(&list_output.stdout).unwrap().trim()));
        });

        spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(r.is_ok());
    }

    #[test]
    fn test_uri() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let output = spawn_virsh(&["uri"]).unwrap().wait_with_output().unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert_eq!(
            std::str::from_utf8(&output.stdout).unwrap().trim(),
            "ch:///system"
        );
    }

    #[test]
    fn test_vm_restart() {
        cleanup_libvirt_state();
        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
        let guest = Guest::new(&mut disk, KernelType::RustFw);

        let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);

        let r = std::panic::catch_unwind(|| {
            spawn_virsh(&["create", domain_path.to_str().unwrap()])
                .unwrap()
                .wait_with_output()
                .unwrap();

            guest.wait_vm_boot(None).unwrap();
            guest.ssh_command("sudo reboot").unwrap();
            guest.wait_vm_boot(None).unwrap();
            let reboot_count = guest
                .ssh_command("sudo journalctl | grep -c -- \"-- Reboot --\"")
                .unwrap()
                .trim()
                .parse::<u32>()
                .unwrap_or_default();
            assert_eq!(reboot_count, 1);

            let list_output = spawn_virsh(&["list", "--all"])
                .unwrap()
                .wait_with_output()
                .unwrap();

            let re = Regex::new(&format!(r"\s+\d+\s+{}\s+running", guest.vm_name)).unwrap();
            assert!(re.is_match(std::str::from_utf8(&list_output.stdout).unwrap().trim()));
        });

        spawn_virsh(&["destroy", &guest.vm_name])
            .unwrap()
            .wait()
            .unwrap();

        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();

        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        );

        assert!(r.is_ok());
    }

    #[test]
    fn test_bridge_networking() {
        use std::fs;
        use std::io::Write;
        use std::process::Command;
        cleanup_libvirt_state();

        //configure host IP  since we cant do it from XML for bridge network.
        Command::new("sh")
                .arg("-c")
                .arg("sudo ip addr add 192.168.1.1/24 dev mybr0 ; sudo ip addr add 192.168.2.1/24 dev mybr0")
                .spawn()
                .expect("Failed to set IP address.");

        let mut guest_name: [String; 2] = Default::default();
        let mut guest_ip: [String; 2] = Default::default();

        let mut libvirtd = spawn_libvirtd().unwrap();
        thread::sleep(std::time::Duration::new(5, 0));

        for i in 0..2 {
            let mut disk = UbuntuDiskConfig::new(FOCAL_IMAGE_NAME.to_owned());
            let guest = Guest::new(&mut disk, KernelType::RustFw);
            let domain_path = guest.create_domain(VcpuConfig::default(), DEFAULT_RAM_SIZE);

            let mut contents =
                fs::read_to_string(&domain_path).expect("Something went wrong reading the file");
            contents = contents.replace("ethernet", "bridge");
            contents = contents.replace("</source>", "");
            contents = contents.replace("source><ip ", "source bridge=\"mybr0\" ");

            let mut f = std::fs::File::create(&domain_path).unwrap();
            f.write_all(contents.as_bytes()).unwrap();

            let r = std::panic::catch_unwind(|| {
                spawn_virsh(&["create", domain_path.to_str().unwrap()])
                    .unwrap()
                    .wait()
                    .unwrap();
                guest.wait_vm_boot(None).unwrap();
            });

            let guestname = &guest.vm_name;
            guest_name[i] = guestname.to_string();
            assert!(r.is_ok());

            guest_ip[i] = guest
                .ssh_command(
                    "ip route get 8.8.8.8 | sed -n '/src/{s/.*src *\\([^ ]*\\).*/\\1/p;q}'",
                )
                .unwrap()
                .trim_end()
                .to_string();
        }
        
        //ping host to guest vm's
        for i in 0..2 {
            let cmd = format!(
                        "ping -c 4 {} ",
                        &guest_ip[i]
                    );
            let out = Command::new("sh")
                     .arg("-c")
                     .arg(&cmd)
                     .status()
                     .expect("failed to execute ping");

            println!(" ssh out is : {:?}", out.code());
            assert!( out.code() == Some(0));
        }

        for i in 0..2 {
            spawn_virsh(&["destroy", &guest_name[i]])
                .unwrap()
                .wait()
                .unwrap();
        }
        libvirtd.kill().unwrap();
        let libvirtd_output = libvirtd.wait_with_output().unwrap();
        //Cleanup bridge configuration 
        Command::new("sh")
                .arg("-c")
                .arg("sudo ip addr del 192.168.1.1/24 dev mybr0 ; sudo ip addr del 192.168.2.1/24 dev mybr0")
                .spawn()
                .expect("Failed to clean IP address."); 
        eprintln!(
            "libvirtd stdout\n\n{}\n\nlibvirtd stderr\n\n{}",
            std::str::from_utf8(&libvirtd_output.stdout).unwrap(),
            std::str::from_utf8(&libvirtd_output.stderr).unwrap()
        ); 
    }
}
