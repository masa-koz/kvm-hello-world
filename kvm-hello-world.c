#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdint.h>
#include <linux/kvm.h>
#include <linux/psp-sev.h>

/* CR0 bits */
#define CR0_PE 1u
#define CR0_MP (1U << 1)
#define CR0_EM (1U << 2)
#define CR0_TS (1U << 3)
#define CR0_ET (1U << 4)
#define CR0_NE (1U << 5)
#define CR0_WP (1U << 16)
#define CR0_AM (1U << 18)
#define CR0_NW (1U << 29)
#define CR0_CD (1U << 30)
#define CR0_PG (1U << 31)

/* CR4 bits */
#define CR4_VME 1
#define CR4_PVI (1U << 1)
#define CR4_TSD (1U << 2)
#define CR4_DE (1U << 3)
#define CR4_PSE (1U << 4)
#define CR4_PAE (1U << 5)
#define CR4_MCE (1U << 6)
#define CR4_PGE (1U << 7)
#define CR4_PCE (1U << 8)
#define CR4_OSFXSR (1U << 8)
#define CR4_OSXMMEXCPT (1U << 10)
#define CR4_UMIP (1U << 11)
#define CR4_VMXE (1U << 13)
#define CR4_SMXE (1U << 14)
#define CR4_FSGSBASE (1U << 16)
#define CR4_PCIDE (1U << 17)
#define CR4_OSXSAVE (1U << 18)
#define CR4_SMEP (1U << 20)
#define CR4_SMAP (1U << 21)

#define EFER_SCE 1
#define EFER_LME (1U << 8)
#define EFER_LMA (1U << 10)
#define EFER_NXE (1U << 11)

/* 32-bit page directory entry bits */
#define PDE32_PRESENT 1
#define PDE32_RW (1U << 1)
#define PDE32_USER (1U << 2)
#define PDE32_PS (1U << 7)

/* 64-bit page * entry bits */
#define PDE64_PRESENT 1
#define PDE64_RW (1U << 1)
#define PDE64_USER (1U << 2)
#define PDE64_ACCESSED (1U << 5)
#define PDE64_DIRTY (1U << 6)
#define PDE64_PS (1U << 7)
#define PDE64_G (1U << 8)

int sev_enabled = 0;

struct vm
{
	int sys_fd;
	int fd;
	char *mem;
	size_t mem_size;
	size_t image_size;
	int sev_sys_fd;
	int sev_running;
};

void vm_init(struct vm *vm, size_t mem_size)
{
	int api_ver;
	struct kvm_userspace_memory_region memreg;

	struct sev_issue_cmd issue_cmd = {};
	struct kvm_sev_cmd sev_cmd = {};
	struct sev_user_data_status status = {};
	struct kvm_sev_launch_start start = {};
	struct kvm_sev_guest_status guest_status = {};

	if (sev_enabled)
	{
		vm->sev_sys_fd = open("/dev/sev", O_RDWR);
		if (vm->sev_sys_fd < 0)
		{
			perror("open /dev/sev");
			exit(1);
		}

		issue_cmd.cmd = SEV_PLATFORM_STATUS;
		issue_cmd.data = (unsigned long)&status;
		if (ioctl(vm->sev_sys_fd, SEV_ISSUE_CMD, &issue_cmd) < 0)
		{
			perror("SEV_ISSUE_CMD");
			exit(1);
		}
		fprintf(stderr, "api_major: %d, state: %d, build: %d\n",
				status.api_major, status.state, status.build);
	}

	vm->sys_fd = open("/dev/kvm", O_RDWR);
	if (vm->sys_fd < 0)
	{
		perror("open /dev/kvm");
		exit(1);
	}

	api_ver = ioctl(vm->sys_fd, KVM_GET_API_VERSION, 0);
	if (api_ver < 0)
	{
		perror("KVM_GET_API_VERSION");
		exit(1);
	}

	if (api_ver != KVM_API_VERSION)
	{
		fprintf(stderr, "Got KVM api version %d, expected %d\n",
				api_ver, KVM_API_VERSION);
		exit(1);
	}

	vm->fd = ioctl(vm->sys_fd, KVM_CREATE_VM, 0);
	if (vm->fd < 0)
	{
		perror("KVM_CREATE_VM");
		exit(1);
	}

	if (ioctl(vm->fd, KVM_SET_TSS_ADDR, 0xfffbd000) < 0)
	{
		perror("KVM_SET_TSS_ADDR");
		exit(1);
	}

	vm->mem_size = mem_size;
	vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (vm->mem == MAP_FAILED)
	{
		perror("mmap mem");
		exit(1);
	}

	memset(vm->mem, 0, vm->mem_size);
	madvise(vm->mem, mem_size, MADV_MERGEABLE);

	memreg.slot = 0;
	memreg.flags = 0;
	memreg.guest_phys_addr = 0;
	memreg.memory_size = mem_size;
	memreg.userspace_addr = (unsigned long)vm->mem;
	if (ioctl(vm->fd, KVM_SET_USER_MEMORY_REGION, &memreg) < 0)
	{
		perror("KVM_SET_USER_MEMORY_REGION");
		exit(1);
	}

	if (sev_enabled)
	{
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_INIT;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)NULL;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_INIT");
			exit(1);
		}

		memset(&start, 0, sizeof(start));
		start.policy = 0x00;
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_LAUNCH_START;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&start;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_LAUNCH_START");
			exit(1);
		}

		memset(&guest_status, 0, sizeof(guest_status));
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_GUEST_STATUS;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&guest_status;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_GUEST_STATUS");
			exit(1);
		}
		fprintf(stderr, "handle: %d, policy: %d, state: %d\n",
				guest_status.handle, guest_status.policy, guest_status.state);
	}
}

struct vcpu
{
	int fd;
	struct kvm_run *kvm_run;
};

void vcpu_init(struct vm *vm, struct vcpu *vcpu)
{
	int vcpu_mmap_size;

	vcpu->fd = ioctl(vm->fd, KVM_CREATE_VCPU, 0);
	if (vcpu->fd < 0)
	{
		perror("KVM_CREATE_VCPU");
		exit(1);
	}

	vcpu_mmap_size = ioctl(vm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
	if (vcpu_mmap_size <= 0)
	{
		perror("KVM_GET_VCPU_MMAP_SIZE");
		exit(1);
	}

	vcpu->kvm_run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE,
						 MAP_SHARED, vcpu->fd, 0);
	if (vcpu->kvm_run == MAP_FAILED)
	{
		perror("mmap kvm_run");
		exit(1);
	}
}

void dump_image(char *image, size_t image_size)
{
	size_t i;

	fprintf(stderr, "image_size: %lu\n", image_size);
	for (i = 0; i < image_size; i++)
	{
		if (i % 16 == 0)
		{
			fprintf(stderr, "\n");
		}
		else
		{
			fprintf(stderr, ", ");
		}
		fprintf(stderr, "%02X", image[i] & 0x000000FF);
	}
	fprintf(stderr, "\n");
}

int run_vm(struct vm *vm, struct vcpu *vcpu, size_t sz)
{
	struct kvm_regs regs;
	uint64_t memval = 0;
	struct kvm_sev_cmd sev_cmd = {};
	struct kvm_sev_guest_status guest_status = {};
	struct kvm_sev_launch_update_data update_data = {};
	struct kvm_sev_launch_measure measurement = {};
	char *data = NULL;

	if (sev_enabled && vm->sev_running == 0)
	{
		memset(&update_data, 0, sizeof(update_data));
		update_data.uaddr = (__u64)(unsigned long)vm->mem;
		update_data.len = vm->mem_size;
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_LAUNCH_UPDATE_DATA;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&update_data;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_LAUNCH_UPDATE_DATA");
			exit(1);
		}

		memset(&measurement, 0, sizeof(measurement));
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_LAUNCH_MEASURE;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&measurement;
		ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd);
		if (measurement.len == 0)
		{
			exit(1);
		}
		fprintf(stderr, "error=%d, measurement->len=%d\n", sev_cmd.error, measurement.len);

		data = malloc(measurement.len);
		if (data == NULL)
		{
			perror("malloc");
			exit(1);
		}
		measurement.uaddr = (__u64)(unsigned long)data;
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_LAUNCH_MEASURE;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&measurement;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_LAUNCH_MEASURE");
			exit(1);
		}
		free(data);

		memset(&guest_status, 0, sizeof(guest_status));
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_GUEST_STATUS;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&guest_status;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_GUEST_STATUS");
			exit(1);
		}
		fprintf(stderr, "handle: %d, policy: %d, state: %d\n",
				guest_status.handle, guest_status.policy, guest_status.state);

		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_LAUNCH_FINISH;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)0;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_LAUNCH_FINISH");
			exit(1);
		}
		memset(&guest_status, 0, sizeof(guest_status));
		memset(&sev_cmd, 0, sizeof(sev_cmd));
		sev_cmd.id = KVM_SEV_GUEST_STATUS;
		sev_cmd.sev_fd = vm->sev_sys_fd;
		sev_cmd.data = (__u64)(unsigned long)&guest_status;
		if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
		{
			perror("KVM_SEV_GUEST_STATUS");
			exit(1);
		}
		fprintf(stderr, "handle: %d, policy: %d, state: %d\n",
				guest_status.handle, guest_status.policy, guest_status.state);

		vm->sev_running = 1;
	}

	if (sev_enabled)
	{
		char *image = NULL;
		fprintf(stderr, "Dump Encrypted image:\n");

		dump_image(vm->mem, vm->image_size);

		image = malloc(vm->image_size);
		if (image != NULL)
		{
			struct kvm_sev_dbg sev_dbg;

			memset(&sev_dbg, 0, sizeof(sev_dbg));
			sev_dbg.src_uaddr = (__u64)(unsigned long)vm->mem;
			sev_dbg.dst_uaddr = (__u64)(unsigned long)image;
			sev_dbg.len = vm->image_size;
			memset(&sev_cmd, 0, sizeof(sev_cmd));
			sev_cmd.id = KVM_SEV_DBG_DECRYPT;
			sev_cmd.sev_fd = vm->sev_sys_fd;
			sev_cmd.data = (__u64)(unsigned long)&sev_dbg;
			if (ioctl(vm->fd, KVM_MEMORY_ENCRYPT_OP, &sev_cmd) < 0)
			{
				perror("KVM_SEV_DBG_DECRYPT");
				exit(1);
			}
			fprintf(stderr, "Dump decrypted image:\n");
			dump_image(image, vm->image_size);
		}
	}

	for (;;)
	{
		if (ioctl(vcpu->fd, KVM_RUN, 0) < 0)
		{
			perror("KVM_RUN");
			exit(1);
		}

		switch (vcpu->kvm_run->exit_reason)
		{
		case KVM_EXIT_HLT:
			goto check;

		case KVM_EXIT_IO:
			if (vcpu->kvm_run->io.direction == KVM_EXIT_IO_OUT && vcpu->kvm_run->io.port == 0xE9)
			{
				char *p = (char *)vcpu->kvm_run;
				fwrite(p + vcpu->kvm_run->io.data_offset,
					   vcpu->kvm_run->io.size, 1, stdout);
				fflush(stdout);
				continue;
			}
			if (vcpu->kvm_run->io.direction == KVM_EXIT_IO_IN && vcpu->kvm_run->io.port == 0xE9)
			{
				fprintf(stderr, "inb\n");
				*(volatile uint8_t *)((uintptr_t)vcpu->kvm_run + vcpu->kvm_run->io.data_offset) = 0x00;
				continue;
			}

			/* fall through */
		default:
			fprintf(stderr, "Got exit_reason %d,"
							" expected KVM_EXIT_HLT (%d)\n",
					vcpu->kvm_run->exit_reason, KVM_EXIT_HLT);
			exit(1);
		}
	}

check:
	if (ioctl(vcpu->fd, KVM_GET_REGS, &regs) < 0)
	{
		perror("KVM_GET_REGS");
		exit(1);
	}

	if (regs.rax != 42)
	{
		printf("Wrong result: {E,R,}AX is %lld\n", regs.rax);
		return 0;
	}

	memcpy(&memval, &vm->mem[0x400], sz);
	fprintf(stderr, "memory at 0x400 is %llx\n",
			(unsigned long long)memval);
	if (sev_enabled == 0 && memval != 42)
	{
		printf("Wrong result: memory at 0x400 is %lld\n",
			   (unsigned long long)memval);
		return 0;
	}

	return 1;
}

extern const unsigned char guest16[], guest16_end[];

int run_real_mode(struct vm *vm, struct vcpu *vcpu)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;

	printf("Testing real mode\n");

	if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
	{
		perror("KVM_GET_SREGS");
		exit(1);
	}

	sregs.cs.selector = 0;
	sregs.cs.base = 0;

	if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
	{
		perror("KVM_SET_SREGS");
		exit(1);
	}

	memset(&regs, 0, sizeof(regs));
	/* Clear all FLAGS bits, except bit 1 which is always set. */
	regs.rflags = 2;
	regs.rip = 0;

	if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
	{
		perror("KVM_SET_REGS");
		exit(1);
	}

	vm->image_size = guest16_end - guest16;
	memcpy(vm->mem, guest16, vm->image_size);
	return run_vm(vm, vcpu, 2);
}

static void setup_protected_mode(struct kvm_sregs *sregs)
{
	struct kvm_segment seg = {
		.base = 0,
		.limit = 0xffffffff,
		.selector = 1 << 3,
		.present = 1,
		.type = 11, /* Code: execute, read, accessed */
		.dpl = 0,
		.db = 1,
		.s = 1, /* Code/data */
		.l = 0,
		.g = 1, /* 4KB granularity */
	};

	sregs->cr0 |= CR0_PE; /* enter protected mode */

	sregs->cs = seg;

	seg.type = 3; /* Data: read/write, accessed */
	seg.selector = 2 << 3;
	sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = seg;
}

extern const unsigned char guest32[], guest32_end[];

int run_protected_mode(struct vm *vm, struct vcpu *vcpu)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	uint32_t memval;

	printf("Testing protected mode\n");

	if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
	{
		perror("KVM_GET_SREGS");
		exit(1);
	}

	setup_protected_mode(&sregs);

	if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
	{
		perror("KVM_SET_SREGS");
		exit(1);
	}

	memset(&regs, 0, sizeof(regs));
	/* Clear all FLAGS bits, except bit 1 which is always set. */
	regs.rflags = 2;
	regs.rip = 0;

	if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
	{
		perror("KVM_SET_REGS");
		exit(1);
	}

	memval = 0xFFFFFFFF;
	memcpy(&vm->mem[0x400], &memval, 4);
	vm->image_size = guest32_end - guest32;
	memcpy(vm->mem, guest32, vm->image_size);
	return run_vm(vm, vcpu, 4);
}

static void setup_paged_32bit_mode(struct vm *vm, struct kvm_sregs *sregs)
{
	uint32_t pd_addr = 0x2000;
	uint32_t *pd = (void *)(vm->mem + pd_addr);

	/* A single 4MB page to cover the memory region */
	pd[0] = PDE32_PRESENT | PDE32_RW | PDE32_USER | PDE32_PS;
	/* Other PDEs are left zeroed, meaning not present. */

	sregs->cr3 = pd_addr;
	sregs->cr4 = CR4_PSE;
	sregs->cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_WP | CR0_AM | CR0_PG;
	sregs->efer = 0;
}

int run_paged_32bit_mode(struct vm *vm, struct vcpu *vcpu)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	uint32_t memval;

	printf("Testing 32-bit paging\n");

	if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
	{
		perror("KVM_GET_SREGS");
		exit(1);
	}

	setup_protected_mode(&sregs);
	setup_paged_32bit_mode(vm, &sregs);

	if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
	{
		perror("KVM_SET_SREGS");
		exit(1);
	}

	memset(&regs, 0, sizeof(regs));
	/* Clear all FLAGS bits, except bit 1 which is always set. */
	regs.rflags = 2;
	regs.rip = 0;

	if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
	{
		perror("KVM_SET_REGS");
		exit(1);
	}

	memval = 0xFFFFFFFF;
	memcpy(&vm->mem[0x400], &memval, 4);

	vm->image_size = guest32_end - guest32;
	memcpy(vm->mem, guest32, vm->image_size);
	return run_vm(vm, vcpu, 4);
}

extern const unsigned char guest64[], guest64_end[];

static void setup_64bit_code_segment(struct kvm_sregs *sregs)
{
	struct kvm_segment seg = {
		.base = 0,
		.limit = 0xffffffff,
		.selector = 1 << 3,
		.present = 1,
		.type = 11, /* Code: execute, read, accessed */
		.dpl = 0,
		.db = 0,
		.s = 1, /* Code/data */
		.l = 1,
		.g = 1, /* 4KB granularity */
	};

	sregs->cs = seg;

	seg.type = 3; /* Data: read/write, accessed */
	seg.selector = 2 << 3;
	sregs->ds = sregs->es = sregs->fs = sregs->gs = sregs->ss = seg;
}

static void setup_long_mode(struct vm *vm, struct kvm_sregs *sregs)
{
	uint64_t pml4_addr = 0x2000;
	uint64_t *pml4 = (void *)(vm->mem + pml4_addr);

	uint64_t pdpt_addr = 0x3000;
	uint64_t *pdpt = (void *)(vm->mem + pdpt_addr);

	uint64_t pd_addr = 0x4000;
	uint64_t *pd = (void *)(vm->mem + pd_addr);

	pml4[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pdpt_addr;
	pdpt[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | pd_addr;
	pd[0] = PDE64_PRESENT | PDE64_RW | PDE64_USER | PDE64_PS;

	sregs->cr3 = pml4_addr;
	sregs->cr4 = CR4_PAE;
	sregs->cr0 = CR0_PE | CR0_MP | CR0_ET | CR0_NE | CR0_WP | CR0_AM | CR0_PG;
	sregs->efer = EFER_LME | EFER_LMA;

	setup_64bit_code_segment(sregs);
}

int run_long_mode(struct vm *vm, struct vcpu *vcpu)
{
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	uint64_t memval;

	printf("Testing 64-bit mode\n");

	if (ioctl(vcpu->fd, KVM_GET_SREGS, &sregs) < 0)
	{
		perror("KVM_GET_SREGS");
		exit(1);
	}

	setup_long_mode(vm, &sregs);

	if (ioctl(vcpu->fd, KVM_SET_SREGS, &sregs) < 0)
	{
		perror("KVM_SET_SREGS");
		exit(1);
	}

	memset(&regs, 0, sizeof(regs));
	/* Clear all FLAGS bits, except bit 1 which is always set. */
	regs.rflags = 2;
	regs.rip = 0;
	/* Create stack at top of 2 MB page and grow down. */
	regs.rsp = 2 << 20;

	if (ioctl(vcpu->fd, KVM_SET_REGS, &regs) < 0)
	{
		perror("KVM_SET_REGS");
		exit(1);
	}

	vm->image_size = guest64_end - guest64;
	memcpy(vm->mem, guest64, vm->image_size);
	memval = 0xFFFFFFFFFFFF;
	memcpy(&vm->mem[0x400], &memval, 8);
	return run_vm(vm, vcpu, 8);
}

int main(int argc, char **argv)
{
	struct vm vm;
	struct vcpu vcpu;
	enum
	{
		REAL_MODE,
		PROTECTED_MODE,
		PAGED_32BIT_MODE,
		LONG_MODE,
	} mode = REAL_MODE;
	int opt;

	while ((opt = getopt(argc, argv, "rsple")) != -1)
	{
		switch (opt)
		{
		case 'e':
			sev_enabled = 1;
			break;

		case 'r':
			mode = REAL_MODE;
			break;

		case 's':
			mode = PROTECTED_MODE;
			break;

		case 'p':
			mode = PAGED_32BIT_MODE;
			break;

		case 'l':
			mode = LONG_MODE;
			break;

		default:
			fprintf(stderr, "Usage: %s [ -r | -s | -p | -l ]\n",
					argv[0]);
			return 1;
		}
	}

	fprintf(stderr, "SEV %s\n", sev_enabled ? "ENABLED" : "DISABLED");
	memset(&vm, 0, sizeof(vm));
	vm_init(&vm, 0x200000);
	vcpu_init(&vm, &vcpu);

	switch (mode)
	{
	case REAL_MODE:
		return !run_real_mode(&vm, &vcpu);

	case PROTECTED_MODE:
		return !run_protected_mode(&vm, &vcpu);

	case PAGED_32BIT_MODE:
		return !run_paged_32bit_mode(&vm, &vcpu);

	case LONG_MODE:
		return !run_long_mode(&vm, &vcpu);
	}

	return 1;
}
