# readwrite-kernel-stable
edit: hello, this project is deprecated and will not be updated anymore, these are also poorly written and unlikely to be really stable in any situation

update: added search process modules into the driver

a more stable &amp; secure read/write virtual memory for kernel mode drivers

intended to be called using Kernel Function hooks, but could be fitted to be used with IOCTL's

these method's are slightly more secure compared to paracorded's methods (links in driver.c) and should be faster then MmCopyVirtualMemory

created with the purpose of removing PAGE_FAULT_IN_NONPAGED_AREA bluescreen, and any other BSOD's that come with copying bad memory
