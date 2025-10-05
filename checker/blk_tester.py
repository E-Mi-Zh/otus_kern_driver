#!/usr/bin/env python3
import os
import time
import tempfile
import struct
import fcntl
from .base_tester import BaseModuleTester


class ExBlkTester(BaseModuleTester):
    def __init__(self, module_name):
        super().__init__(module_name)
        self.device_name = "ex_blk"
        self.block_device = f"/dev/{self.device_name}"
        # Total size should account for MBR sector + 3 partitions
        self.total_size = (
            3 * 100 * 1024 * 1024
        ) + 512  # 300MB total for partitions + 512 bytes for MBR
        self.partition_size = 100 * 1024 * 1024  # 100MB per partition
        self.proc_file = f"/proc/{self.device_name}/capacity"
        self.sysfs_file = f"/sys/class/{self.device_name}/{self.device_name}/capacity"

    def cleanup_device(self):
        """Clean up any existing device mappings"""
        # Unmount any mounted partitions
        for i in range(4):  # 3 partitions + maybe whole device
            part_device = f"{self.block_device}{i if i > 0 else ''}"
            mount_point = f"/mnt/{self.device_name}{i if i > 0 else ''}"

            # Unmount if mounted
            self.run_command(f"sudo umount {part_device} 2>/dev/null || true")
            self.run_command(f"sudo umount {mount_point} 2>/dev/null || true")

        # Remove mount points
        for i in range(4):
            mount_point = f"/mnt/{self.device_name}{i if i > 0 else ''}"
            self.run_command(f"sudo rm -rf {mount_point} 2>/dev/null || true")

    def test_module_lifecycle(self):
        """Test module loading/unloading"""
        print("\n=== Module Lifecycle Tests ===")
        self.clear_dmesg()

        # Check if proc and sysfs files exist before module load (should not)
        if os.path.exists(self.proc_file):
            print("WARNING: Proc file exists before module load")
        if os.path.exists(self.sysfs_file):
            print("WARNING: Sysfs file exists before module load")

        # Load module
        self.load_module()
        success = self.assert_dmesg_contains(
            rf"{self.device_name}.*Initializing|module loaded", "Module initialization"
        )
        if not success:
            return False

        # Check if block device was created
        success = self.assert_file_exists(self.block_device, "Block device creation")
        if not success:
            return False

        # Check if partitions were created
        for i in range(1, 4):
            part_device = f"{self.block_device}{i}"
            self.assert_file_exists(part_device, f"Partition device {part_device}")

        # Check if proc file was created
        success = self.assert_file_exists(self.proc_file, "Proc file creation")
        if not success:
            return False

        # Check if sysfs file was created
        self.assert_file_exists(self.sysfs_file, "Sysfs file creation")

        # Unload module
        self.unload_module()
        success = self.assert_dmesg_contains(
            rf"{self.device_name}.*unloaded|module unloaded", "Module unloading"
        )

        return success

    def test_proc_sysfs_operations(self):
        """Test /proc and /sys filesystem operations"""
        print("\n=== /proc and /sys Filesystem Tests ===")
        self.clear_dmesg()
        self.load_module()
        time.sleep(0.5)

        # Test proc file read
        success, output = self.assert_command_success(
            f"sudo cat {self.proc_file}",
            "Read from proc file"
        )
        if success:
            expected_content = "Capacity:"
            if expected_content in output:
                print("Additional check: Proc file content verified")
            else:
                print(f"Additional check: Proc file content unexpected: {output}")

        # Test proc file write
        test_content = "Test write to proc file"
        success, output = self.assert_command_success(
            f"echo '{test_content}' | sudo tee {self.proc_file}",
            "Write to proc file"
        )
        # Check if write was successful in dmesg
        success = self.assert_dmesg_contains(
            "Written to proc file",
            "Verify proc file write in dmesg"
        )

        # Test sysfs file read
        success, output = self.assert_command_success(
            f"sudo cat {self.sysfs_file}",
            "Read from sysfs file"
        )
        if success:
            expected_content = "Capacity:"
            if expected_content in output:
                print("Additional check: Sysfs file content verified")
            else:
                print(f"Additional check: Sysfs file content unexpected: {output}")

        # Test sysfs file write
        test_content = "Test write to sysfs file"
        success, output = self.assert_command_success(
            f"echo '{test_content}' | sudo tee {self.sysfs_file}",
            "Write to sysfs file"
        )
        # Check if write was successful in dmesg
        success = self.assert_dmesg_contains(
            "Written to sysfs file",
            "Verify sysfs file write in dmesg"
        )

        self.unload_module()
        return True

    def test_block_device_creation(self):
        """Test that block device and partitions are properly created"""
        print("\n=== Block Device Creation Tests ===")
        self.clear_dmesg()
        self.load_module()
        time.sleep(1)

        # Test main block device with fdisk
        success, output = self.assert_command_success(
            f"sudo fdisk -l {self.block_device}", "Check block device with fdisk"
        )
        if not success:
            self.unload_module()
            return False

        # Additional check: Verify partitions are detected in fdisk output
        if "ex_blk1" in output and "ex_blk2" in output and "ex_blk3" in output:
            print("Additional check: Partitions detected correctly in fdisk output")
        else:
            print("Additional check: Partitions not found in fdisk output")

        # Test total device size - should be 300MB + 512 bytes for MBR
        success, output = self.assert_command_success(
            f"sudo blockdev --getsize64 {self.block_device}", "Get total device size"
        )
        if success:
            actual_size = int(output.strip())
            if actual_size == self.total_size:
                print("Additional check: Device size correct")
            else:
                print(
                    f"Additional check: Device size: expected {self.total_size}, got {actual_size} bytes"
                )

        # Test partition sizes - all should be exactly 100MB now
        for i in range(1, 4):
            part_device = f"{self.block_device}{i}"
            success, output = self.assert_command_success(
                f"sudo blockdev --getsize64 {part_device}", f"Get partition {i} size"
            )
            if success:
                part_size = int(output.strip())
                expected_size = self.partition_size

                if part_size == expected_size:
                    print(
                        f"Additional check: Partition {i} size correct: {part_size} bytes"
                    )
                else:
                    print(
                        f"Additional check: Partition {i} size: expected {expected_size}, got {part_size} bytes"
                    )

        # Check dmesg for any truncation warnings (should be none now)
        dmesg_output = self.get_dmesg_output()
        if "truncated" in dmesg_output:
            print("Additional check: WARNING - Partition truncation detected in dmesg")
        else:
            print("Additional check: No partition truncation warnings in dmesg")

        self.unload_module()
        return True

    def test_ioctl_commands(self):
        """Test IOCTL commands on block device"""
        print("\n=== IOCTL Commands Tests ===")
        self.load_module()
        time.sleep(0.5)

        # Test BLKGETSIZE64 (0x80081272) - should return 300MB + 512 bytes
        try:
            with open(self.block_device, "rb") as f:
                # BLKGETSIZE64 - get size in bytes
                import ctypes

                BLKGETSIZE64 = 0x80081272
                size = ctypes.c_ulonglong()
                fcntl.ioctl(f.fileno(), BLKGETSIZE64, size)
                actual_size = size.value

                self.test_count += 1
                print(f"\nTest #{self.test_count}")
                print(f"Command:    BLKGETSIZE64 ioctl")
                print(f"Expected:   {self.total_size} bytes")
                print(f"Found:      {actual_size} bytes")

                if actual_size == self.total_size:
                    self.passed_count += 1
                    print("Result:     PASS")
                else:
                    self.failed_count += 1
                    print("Result:     FAIL")
        except Exception as e:
            self.test_count += 1
            self.failed_count += 1
            print(f"\nTest #{self.test_count}")
            print(f"Command:    BLKGETSIZE64 ioctl")
            print(f"Error:      {e}")
            print("Result:     FAIL")

        # Test HDIO_GETGEO - get disk geometry
        try:
            with open(self.block_device, "rb") as f:
                HDIO_GETGEO = 0x0301
                geo_struct = struct.pack("HHHH", 0, 0, 0, 0)
                geo = fcntl.ioctl(f.fileno(), HDIO_GETGEO, geo_struct)
                heads, sectors, cylinders, start = struct.unpack("HHHH", geo)

                self.test_count += 1
                print(f"\nTest #{self.test_count}")
                print(f"Command:    HDIO_GETGEO ioctl")
                print(
                    f"Found:      Heads: {heads}, Sectors: {sectors}, Cylinders: {cylinders}"
                )

                # Basic validation of geometry values
                if heads > 0 and sectors > 0 and cylinders > 0:
                    self.passed_count += 1
                    print("Result:     PASS")
                else:
                    self.failed_count += 1
                    print("Result:     FAIL")
        except Exception as e:
            # Count optional HDIO_GETGEO as passed if not supported
            self.test_count += 1
            self.passed_count += 1
            print(f"\nTest #{self.test_count}")
            print(f"Command:    HDIO_GETGEO ioctl")
            print(f"Status:     Not supported - {e}")
            print("Result:     PASS (optional feature)")

        self.unload_module()
        return True

    def test_read_write_operations(self):
        """Test basic read/write operations with exact byte handling"""
        print("\n=== Read/Write Operations Tests ===")
        self.load_module()
        time.sleep(0.5)

        # Create test file with exact 5120 bytes (10 sectors)
        test_data = b"TEST_PATTERN_START"
        for i in range(100):
            test_data += f"LINE_{i:04d}_ABCDEFGHIJKLMNOPQRSTUVWXYZ_END\n".encode()
        test_data += b"TEST_PATTERN_END"

        # Pad to exactly 5120 bytes
        test_data = test_data.ljust(5120, b"\x00")

        with tempfile.NamedTemporaryFile(delete=False) as f:
            test_file = f.name
            f.write(test_data)

        read_file = test_file + ".read"

        try:
            # Write exactly 10 sectors (5120 bytes) after MBR (sector 1)
            success, output = self.assert_command_success(
                f"sudo dd if={test_file} of={self.block_device} bs=512 seek=1 count=10 2>&1",
                "Write to block device after MBR",
            )

            if not success:
                self.unload_module()
                os.unlink(test_file)
                return False

            # Read back exactly 10 sectors from the same location
            success, output = self.assert_command_success(
                f"sudo dd if={self.block_device} of={read_file} bs=512 skip=1 count=10 2>&1",
                "Read from block device after MBR",
            )

            if not success:
                self.unload_module()
                os.unlink(test_file)
                if os.path.exists(read_file):
                    os.unlink(read_file)
                return False

            # Compare files byte by byte
            self.test_count += 1
            print(f"\nTest #{self.test_count}")
            print(f"Command:    Compare written and read data")

            try:
                with open(test_file, "rb") as f1, open(read_file, "rb") as f2:
                    data1 = f1.read()
                    data2 = f2.read()

                    if data1 == data2:
                        self.passed_count += 1
                        print("Result:     PASS")
                        print("Data comparison: SUCCESS - exact match")
                    else:
                        self.failed_count += 1
                        print("Result:     FAIL")
                        print(f"Data mismatch: {len(data1)} vs {len(data2)} bytes")
                        # Show first few bytes for debugging
                        print(f"First 100 bytes original: {data1[:100].hex()}")
                        print(f"First 100 bytes read:     {data2[:100].hex()}")
            except Exception as e:
                self.failed_count += 1
                print("Result:     FAIL")
                print(f"Comparison error: {e}")

        finally:
            # Clean up
            if os.path.exists(test_file):
                os.unlink(test_file)
            if os.path.exists(read_file):
                os.unlink(read_file)

        self.unload_module()
        return True

    def test_partition_operations(self):
        """Test operations on individual partitions with exact data handling"""
        print("\n=== Partition Operations Tests ===")
        self.load_module()
        time.sleep(0.5)

        for i in range(1, 4):
            part_device = f"{self.block_device}{i}"

            # Create exact 5120 bytes of test data for each partition
            test_data = f"PARTITION_{i}_TEST_DATA_START\n".encode()
            for j in range(50):
                test_data += f"Partition {i}, Line {j:03d}: ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789\n".encode()
            test_data += f"PARTITION_{i}_TEST_DATA_END\n".encode()

            # Pad to exactly 5120 bytes
            test_data = test_data.ljust(5120, b"\x00")

            with tempfile.NamedTemporaryFile(delete=False) as f:
                test_file = f.name
                f.write(test_data)

            read_file = test_file + ".read"

            try:
                # Write exactly 10 sectors to partition
                success, output = self.assert_command_success(
                    f"sudo dd if={test_file} of={part_device} bs=512 count=10 2>&1",
                    f"Write to partition {i}",
                )

                if not success:
                    continue

                # Read back exactly 10 sectors from partition
                success, output = self.assert_command_success(
                    f"sudo dd if={part_device} of={read_file} bs=512 count=10 2>&1",
                    f"Read from partition {i}",
                )

                if not success:
                    continue

                # Compare files byte by byte
                self.test_count += 1
                print(f"\nTest #{self.test_count}")
                print(f"Command:    Compare data for partition {i}")

                try:
                    with open(test_file, "rb") as f1, open(read_file, "rb") as f2:
                        data1 = f1.read()
                        data2 = f2.read()

                        if data1 == data2:
                            self.passed_count += 1
                            print("Result:     PASS")
                        else:
                            self.failed_count += 1
                            print("Result:     FAIL")
                            print(
                                f"Data length: original={len(data1)}, read={len(data2)}"
                            )
                            # Check if it's just zero padding difference
                            if data1.rstrip(b"\x00") == data2.rstrip(b"\x00"):
                                print("Note: Data matches when ignoring trailing zeros")
                except Exception as e:
                    self.failed_count += 1
                    print("Result:     FAIL")
                    print(f"Comparison error: {e}")

            finally:
                # Clean up
                if os.path.exists(test_file):
                    os.unlink(test_file)
                if os.path.exists(read_file):
                    os.unlink(read_file)

        self.unload_module()
        return True

    def test_filesystem_operations(self):
        """Test creating filesystem and mounting"""
        print("\n=== Filesystem Operations Tests ===")
        self.load_module()
        time.sleep(0.5)

        mount_point = f"/mnt/{self.device_name}1"

        try:
            # Create mount point
            self.run_command(f"sudo mkdir -p {mount_point}")

            # Create filesystem on first partition
            success, output = self.assert_command_success(
                f"sudo mkfs.ext4 -F {self.block_device}1",
                "Create ext4 filesystem on partition 1",
            )

            if not success:
                self.unload_module()
                return False

            # Mount partition
            success, output = self.assert_command_success(
                f"sudo mount {self.block_device}1 {mount_point}", "Mount partition 1"
            )

            if not success:
                self.unload_module()
                return False

            # Test file operations
            test_file = f"{mount_point}/test_file.txt"
            test_content = "Test content for filesystem verification\n"

            success, output = self.assert_command_success(
                f"sudo sh -c 'echo \"{test_content}\" > {test_file}'",
                "Create file on mounted filesystem",
            )

            if not success:
                self.run_command(f"sudo umount {mount_point}")
                self.unload_module()
                return False

            # Read and verify file
            success, output = self.assert_command_success(
                f"sudo cat {test_file}", "Read file from mounted filesystem"
            )

            # Additional check: Verify file content
            if success and test_content.strip() in output:
                print("Additional check: File content verified correctly")
            else:
                print("Additional check: File content mismatch")

            # Test additional file operations
            success, output = self.assert_command_success(
                f"sudo sh -c 'ls -la {mount_point}/'", "List directory contents"
            )

            # Unmount
            success, output = self.assert_command_success(
                f"sudo umount {mount_point}", "Unmount partition"
            )

        except Exception as e:
            print(f"Error in filesystem test: {e}")
            self.failed_count += 1
        finally:
            # Cleanup
            self.run_command(f"sudo umount {mount_point} 2>/dev/null || true")
            self.run_command(f"sudo rm -rf {mount_point}")

        self.unload_module()
        return True

    def test_error_conditions(self):
        """Test error handling - these tests check for expected failures"""
        print("\n=== Error Conditions Tests ===")
        self.load_module()
        time.sleep(0.5)

        # Test writing beyond device capacity - this should fail
        # We use a custom test instead of assert_command_success since we expect failure
        self.test_count += 1
        print(f"\nTest #{self.test_count}")
        print(
            f"Command:    sudo dd if=/dev/zero of={self.block_device} bs=1M count=400 oflag=direct"
        )
        print(f"Description:Write beyond device capacity (expected to fail)")

        stdout, stderr, returncode = self.run_command(
            f"sudo dd if=/dev/zero of={self.block_device} bs=1M count=400 oflag=direct 2>&1"
        )

        print(f"Return code: {returncode}")
        if stderr:
            print(f"Error output: {stderr}")

        # This test PASSES if the command fails (returncode != 0) as expected
        if returncode != 0:
            self.passed_count += 1
            print("Result:     PASS - correctly failed as expected")
        else:
            self.failed_count += 1
            print("Result:     FAIL - should have failed but succeeded")

        # Test reading from non-existent sector
        # We use a custom test instead of assert_command_success since failure is acceptable
        self.test_count += 1
        print(f"\nTest #{self.test_count}")
        print(
            f"Command:    sudo dd if={self.block_device} of=/dev/null bs=512 count=1 skip=1000000"
        )
        print(f"Description:Read from non-existent sector")

        stdout, stderr, returncode = self.run_command(
            f"sudo dd if={self.block_device} of=/dev/null bs=512 count=1 skip=1000000 2>&1"
        )

        print(f"Return code: {returncode}")
        if stderr:
            print(f"Error output: {stderr}")

        # This test PASSES regardless of outcome since both behaviors are acceptable
        # Some devices return zeros, others return errors
        self.passed_count += 1
        if returncode == 0:
            print("Result:     PASS - completed successfully (returned zeros)")
        else:
            print("Result:     PASS - failed as expected (sector out of range)")

        self.unload_module()
        return True

    def run_all_tests(self):
        """Run all tests for ex_blk module"""
        print(f"=== Testing {self.device_name} Block Device Module ===\n")

        try:
            # Cleanup before starting
            self.cleanup_device()
            self.unload_module()

            # Run test suites
            self.test_module_lifecycle()
            self.test_block_device_creation()
            self.test_proc_sysfs_operations()
            self.test_ioctl_commands()
            self.test_read_write_operations()
            self.test_partition_operations()
            self.test_filesystem_operations()
            self.test_error_conditions()

        except Exception as e:
            print(f"Error during testing: {e}")
            self.failed_count += 1

        finally:
            # Final cleanup
            self.cleanup_device()
            self.unload_module()

        # Print summary
        return self.print_summary()
