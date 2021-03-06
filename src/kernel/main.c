#include <include/mman.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu/exception.h"
#include "cpu/gdt.h"
#include "cpu/hal.h"
#include "cpu/idt.h"
#include "cpu/pit.h"
#include "cpu/rtc.h"
#include "cpu/tss.h"
#include "devices/ata.h"
#include "devices/char/memory.h"
#include "devices/char/tty.h"
#include "devices/kybrd.h"
#include "devices/mouse.h"
#include "devices/pci.h"
#include "fs/ext2/ext2.h"
#include "fs/vfs.h"
#include "ipc/message_queue.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "multiboot2.h"
#include "net/devices/rtl8139.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/icmp.h"
#include "net/net.h"
#include "net/tcp.h"
#include "proc/task.h"
#include "system/framebuffer.h"
#include "system/sysapi.h"
#include "system/time.h"
#include "system/timer.h"
#include "utils/math.h"
#include "utils/printf.h"
#include "utils/string.h"

extern struct vfs_file_system_type ext2_fs_type;

void setup_window_server(struct Elf32_Layout *elf_layout)
{
	// setup stdin, stdiout and stderr
	int fd = vfs_open("/dev/null", O_RDWR);
	sys_dup2(fd, 0);
	sys_dup2(fd, 1);
	sys_dup2(fd, 2);

	// map framebuffer to userspace
	struct framebuffer *fb = get_framebuffer();
	uint32_t screen_size = fb->height * fb->pitch;
	struct vm_area_struct *area = get_unmapped_area(0, screen_size);
	uint32_t blocks = (area->vm_end - area->vm_start) / PMM_FRAME_SIZE;
	for (uint32_t iblock = 0; iblock < blocks; ++iblock)
		vmm_map_address(
			current_thread->parent->pdir,
			area->vm_start + iblock * PMM_FRAME_SIZE,
			fb->addr + iblock * PMM_FRAME_SIZE,
			I86_PTE_PRESENT | I86_PTE_WRITABLE | I86_PTE_USER);

	elf_layout->stack -= sizeof(struct framebuffer);
	struct framebuffer *ws_fb = (struct framebuffer *)elf_layout->stack;
	memcpy(ws_fb, fb, sizeof(struct framebuffer));
	ws_fb->addr = area->vm_start;

	// setup argv
	elf_layout->stack -= 4;
	*(uint32_t *)elf_layout->stack = (uint32_t)ws_fb;
	char **argv = (char **)elf_layout->stack;

	setup_user_thread_stack(elf_layout, 1, argv, NULL);
}

void kernel_init()
{
	// when a task switches to kernel/net thread at the first time, it has to call `schedule` (also `lock_scheduler`)
	// -> we miss one `unlock_scheduler` to balance the counter
	// -> trigger manually at the beginning of thread path
	unlock_scheduler();

	timer_init();

	// setup random's seed
	srand(get_seconds(NULL));

	// FIXME: MQ 2019-11-19 ata_init is not called in pci_scan_buses without enabling -O2
	pci_init();
	ata_init();

	vfs_init(&ext2_fs_type, "/dev/hda");
	chrdev_memory_init();
	tty_init();

	/// init keyboard and mouse
	kkybrd_install();
	mouse_init();

	// init ipc message queue
	mq_init();

	// register system apis
	syscall_init();

	process_load("window server", "/bin/window_server", THREAD_SYSTEM_POLICY, 0, setup_window_server);

	// idle
	update_thread(current_thread, THREAD_WAITING);
	schedule();

	for (;;)
		;
}

int kernel_main(uint32_t addr, uint32_t magic)
{
	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
		return -1;

	struct multiboot_tag_basic_meminfo *multiboot_meminfo;
	struct multiboot_tag_mmap *multiboot_mmap;
	struct multiboot_tag_framebuffer *multiboot_framebuffer;

	struct multiboot_tag *tag;
	for (tag = (struct multiboot_tag *)(addr + 8);
		 tag->type != MULTIBOOT_TAG_TYPE_END;
		 tag = (struct multiboot_tag *)((multiboot_uint8_t *)tag + ((tag->size + 7) & ~7)))
	{
		switch (tag->type)
		{
		case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
			multiboot_meminfo = (struct multiboot_tag_basic_meminfo *)tag;
			break;
		case MULTIBOOT_TAG_TYPE_MMAP:
		{
			multiboot_mmap = (struct multiboot_tag_mmap *)tag;
			break;
		}
		case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
		{
			multiboot_framebuffer = (struct multiboot_tag_framebuffer *)tag;
			break;
		}
		}
	}

	// setup serial port A for debug
	debug_init();

	// gdt including kernel, user and tss
	gdt_init();
	install_tss(5, 0x10, 0);

	// register irq and handlers
	idt_init();

	// physical memory and paging
	pmm_init(multiboot_meminfo, multiboot_mmap);
	vmm_init();

	exception_init();

	// timer
	rtc_init();
	pit_init();

	framebuffer_init(multiboot_framebuffer);

	// enable interrupts to start irqs (timer, keyboard)
	enable_interrupts();

	task_init(kernel_init);

	for (;;)
		;

	return 0;
}
