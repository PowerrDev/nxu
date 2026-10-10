#include <nxu/syscall.h>
#include <nxu/string.h>
#include <nxu/dyrld_protocol.h>
#include <frameworks/CoreFoundation.framework/lib/service/bootstrap_client.h>
#include <frameworks/Dyrld.framework/elf.h>
#include <libk/sha256.h>
#include <stdbool.h>

static bool read_exact(uint64_t fd, uint64_t offset, void *buffer, uint64_t size)
{
	if (nxu_seek(fd, offset) < 0)
		return false;

	uint64_t total = 0;

	while (total < size) {
		int64_t n = nxu_read(fd, (char *)buffer + total, size - total);

		if (n <= 0)
			return false;

		total += n;
	}

	return true;
}

static bool measure(nxu_dyrld_measurement_t *request)
{
	int64_t fd = nxu_open(request->path, NXU_O_READ);

	if (fd < 0)
		return false;

	dyr_elf_header_t header;
	dyr_elf_program_t programs[NXU_DYRLD_SEGMENTS];
	bool valid = read_exact(fd, 0, &header, sizeof(header)) && header.ident[0] == 0x7f && header.ident[1] == 'E' &&
				 header.ident[2] == 'L' && header.ident[3] == 'F' && header.ident[4] == DYR_ELF_CLASS &&
				 header.ident[5] == 1 && header.type == 3 && header.machine == DYR_ELF_MACHINE &&
				 header.phentsize == sizeof(programs[0]) && header.phnum > 0 && header.phnum <= NXU_DYRLD_SEGMENTS &&
				 read_exact(fd, header.phoff, programs, header.phnum * sizeof(programs[0]));
	sha256_context_t digest;
	sha256_init(&digest);

	/* Keep large reads off the daemon stack and avoid one seek per page. */
	static char buffer[64 * 1024];

	for (uint32_t i = 0; valid && i < header.phnum; i++)
		if (programs[i].type == 1) {
			if (programs[i].filesz > 32ULL * 1024 * 1024 || programs[i].offset > UINT64_MAX - programs[i].filesz) {
				valid = false;
				break;
			}

			for (uint64_t offset = 0; valid && offset < programs[i].filesz;) {
				uint64_t chunk = programs[i].filesz - offset;

				if (chunk > sizeof(buffer))
					chunk = sizeof(buffer);
				valid = read_exact(fd, programs[i].offset + offset, buffer, chunk);
				if (valid)
					sha256_update(&digest, buffer, chunk);
				offset += chunk;
			}
		}

	if (valid)
		sha256_final(&digest, request->identity);
	(void)nxu_close(fd);
	return valid;
}

int main(void)
{
	int64_t port = nxu_ipc_port_allocate();

	if (port <= 0 || !bootstrap_client_register(NXU_DYRLD_SERVICE, port))
		return 1;

	static const char ready[] = "dyrld: framework measurement service ready\n";
	(void)nxu_write(1, ready, sizeof(ready) - 1);
	for (;;) {
		nxu_dyrld_measurement_t request = {0};
		uint32_t reply = 0, type = 0;
		int64_t n = nxu_ipc_receive_wait(port, &request, sizeof(request), &reply, &type);

		if (n == -NXU_SYS_E_INTERRUPTED)
			continue;

		if (n < 0)
			return 2;

		if (reply == 0 || type != NXU_IPC_XFER_PORT)
			continue;

		request.path[NXU_DYRLD_PATH - 1] = 0;
		request.status = n == sizeof(request) && request.version == NXU_DYRLD_PROTOCOL_VERSION &&
								 request.path[0] == '/' && measure(&request)
							 ? 0
							 : 1;
		(void)nxu_ipc_send(reply, &request, sizeof(request), 0);
		(void)nxu_ipc_port_deallocate(reply);
	}
}
