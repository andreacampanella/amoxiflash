/*
amoxiflash -- NAND Flash chip programmer utility, using the Infectus 1 / 2 chip
Copyright (C) 2008  bushing

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

For more information, contact bushing@gmail.com, or see http://code.google.com/p/amoxiflash
*/

#define VERSION "0.5"

#include <stdio.h>
#include <math.h>
#include <inttypes.h>
#include <usb.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include "amoxiflash.h"

#ifdef __MINGW32__
#define fseeko fseeko64
#define ftello ftello64
#define usleep(x) _sleep((x)/1000)
#endif

#define ENDPOINT_READ 0x81
#define ENDPOINT_WRITE 1

#define INFECTUS_NAND_CMD 0x4e
#define INFECTUS_NAND_SEND 0x1
#define INFECTUS_NAND_RECV 0x2

#define NAND_RESET 0xff
#define NAND_CHIPID 0x90
#define NAND_GETSTATUS 0x70
#define NAND_ERASE_PRE 0x60
#define NAND_ERASE_POST 0xd0
#define NAND_READ_PRE 0x00
#define NAND_READ_POST 0x30
#define NAND_WRITE_PRE 0x80
#define NAND_WRITE_POST 0x10

#define PAGEBUF_SIZE 4096

usb_dev_handle *locate_infectus(void);

struct usb_dev_handle *h;
struct timeval tv1, tv2;
char *progname;

int run_fast = 0;
int subpage_size = 0x2c0;
int page_size = 2048;
int spare_size = 64;
int num_blocks = 4096;
int pages_per_block = 64;
int verify_after_write = 1;
int chip_select = 0;
int force = 0;
int debug_mode = 0;
int test_mode = 0;
int check_status = 0;
int start_block = 0;
int quick_check = 0;

u32 start_time = 0;
u32 blocks_done = 0;

char *spinner_chars="/-\\|";
int spin = 0;

void draw_spin(void) {
	printf("\b%c", spinner_chars[spin++]);
	fflush(stdout);
    if(!spinner_chars[spin]) spin=0;    
}

static const char *pld_ids[] = {
      "O2MOD",
      "Globe Hitachi",
      "Globe Samsung",
      "Infectus 78",
      "NAND Programmer",
      "2 NAND Programmer",
      "SPI Programmer",
      "XDowngrader"
};


void timer_start(void) {
	gettimeofday(&tv1, NULL);
}

unsigned long long timer_end(void) {
	unsigned long long retval;
	gettimeofday(&tv2, NULL);
	retval = (tv2.tv_sec - tv1.tv_sec) * 1000000ULL;
	retval += tv2.tv_usec - tv1.tv_usec;
	return retval;
}

char ascii(char s) {
  if(s < 0x20) return '.';
  if(s > 0x7E) return '.';
  return s;
}

void hexdump(void *d, int len) {
  u8 *data;
  int i, off;
  data = (u8*)d;
  for (off=0; off<len; off += 16) {
    printf("%08x  ",off);
    for(i=0; i<16; i++)
      if((i+off)>=len) printf("   ");
      else printf("%02x ",data[off+i]);

    printf(" ");
    for(i=0; i<16; i++)
      if((i+off)>=len) printf(" ");
      else printf("%c",ascii(data[off+i]));
    printf("\n");
  }
}

int infectus_sendcommand(u8 *buf, int len, int maxsize) {
	if (debug_mode) {
		printf("> "); hexdump(buf, len);
	}
	
	int ret = 0;
start:
		while (ret < len) {
		ret = usb_bulk_write(h, ENDPOINT_WRITE, (char *)buf, len, 500);
		if (ret < 0) {
			printf("Error %d sending command: %s\n", ret, usb_strerror());
			return ret;
		}
		if (ret != len) {
			printf("Error: short write (%d < %d)\n", ret, len);
		}
	}

	ret=usb_bulk_read(h, ENDPOINT_READ, (char *)buf, maxsize, 500);	
	if(ret < 0) {
		printf("Error reading reply: %d\n", ret);
		return ret;		
	}
	
	if (debug_mode) {
		printf("< ");
		hexdump(buf, ret);
	}
	
	if(buf[0] != 0xFF) {
		printf("Reply began with %02x, expected ff\n", buf[0]);
		goto start;
	}
	buf++; // skip initial FF
	
	if (debug_mode && ret > 0) hexdump(buf, ret);
//	usleep(1000);
	return ret;
}

int infectus_nand_command(u8 *command, unsigned int len, ...) {
	int i;
	va_list ap;
    va_start(ap, len);
	memset(command, 0, len+9);
	command[0]=INFECTUS_NAND_CMD;
	command[7]=len;
	for(i=0; i<=len; i++) {
		command[i+8]=va_arg(ap,int) & 0xff;
	}
    va_end(ap);
	return len+9;
}

int infectus_nand_receive(u8 *buf, int len) {
	memset(buf, 0, 8);
	buf[0] = INFECTUS_NAND_CMD;
	buf[1] = INFECTUS_NAND_RECV;
	buf[6] = (len >> 8) & 0xff;
	buf[7] = len & 0xff;
	return infectus_sendcommand(buf, 8, len+3);
}

int infectus_nand_send(u8 *buf, int len) {
	u8 temp_buf[PAGEBUF_SIZE];
	memset(temp_buf, 0, sizeof temp_buf);

	memcpy(temp_buf, "\x4e\x01\x00\x00\x00\x00", 8);
	temp_buf[6] = len/256;
	temp_buf[7] = len%256;
	memcpy(temp_buf+8, buf, len);
	
	int retval = infectus_sendcommand(temp_buf, len+8, PAGEBUF_SIZE);
	if (retval < 0) return retval;
	if (retval > len) retval = len - 1;
	memcpy(buf, temp_buf, retval);
	return retval;
}

int infectus_reset(void) {
	u8 buf[128];
	int ret;
	
	ret = usb_set_configuration(h,1);
	if (ret) printf("conf_stat=%d\n",ret);

	if (ret == -1) {
		printf("Unable to set USB device configuration; are you running as root?\n");
		exit(0);
	}

	/* Initialize "USB stuff" */
	ret = usb_claim_interface(h,0);
	if (ret) printf("claim_stat=%d\n",ret);

	ret = usb_set_altinterface(h,0);
	if (ret) printf("alt_stat=%d\n",ret);
	
	ret = usb_clear_halt(h, ENDPOINT_READ);
	if (ret) printf("usb_clear_halt(%x)=%d (%s)\n", ENDPOINT_READ, ret, usb_strerror());

	/* What does this do? */
	ret = usb_control_msg(h, USB_TYPE_VENDOR + USB_RECIP_DEVICE, 
		2, 2, 0, (char *)buf, 0, 1000);
	if (ret) printf("usb_control_msg(2)=%d\n", ret);

	/* Send Infectus reset command */
	ret = 0;
	while(ret == 0) {
		memcpy(buf, "\x45\x15\x00\x00\x00\x00\x00\x00", 8);
		ret = infectus_sendcommand(buf, 8, 128);
	}
	return 0;
}

/* Version part 1: ? */
int infectus_get_version(void) {
	u8 buf[128];
	int ret;
	memcpy(buf, "\x45\x13\x01\x00\x00\x00\x00\x00", 8);

	ret=infectus_sendcommand(buf, 8, 128);
	printf("Infectus version (?) = %hhx\n", buf[1]);
//	hexdump(buf, ret);
	return 0;
}

/* Version part 2: loader version */
int infectus_get_loader_version(void) {
	u8 buf[128];
	int ret;
	memcpy(buf, "\x4c\x07\x00\x00\x00\x00\x00\x00", 8);

	ret = infectus_sendcommand(buf, 8, 128);
	printf("Infectus Loader version = %hhu.%hhu\n", buf[1], buf[2]);
//	hexdump(buf, ret);
	return 0;
}

int infectus_check_pld_id(void) {
	u8 buf[128];
	int ret;
	memcpy(buf, "\x4c\x15\x00\x00\x00\x00\x00\x00", 8);

	ret = infectus_sendcommand(buf, 8, 128);
	if (ret < 0) return ret;
	if (buf[1] > (sizeof pld_ids)/4) {
		fprintf(stderr, "Unknown PLD ID %d\n", buf[1]);
	} else {
		printf("PLD ID: %s\n", pld_ids[buf[1]]);
	}
/*	if (buf[1] != 7 && buf[1] != 4) {
		fprintf(stderr, "WARNING: If you experience problems, please try reprogramming the Infectus\n");
		fprintf(stderr, "chip with the 'NAND Programmer' or 'XDowngrader' firmwares and try again.\n");
	} */
	return 0;
}

/* In a dual-NAND configuration, select one of the chips (generally 0 or 1) */
int infectus_selectflash(int which) {
	u8 buf[128];
	memcpy(buf, "\x45\x14\x00\x00\x00\x00\x00\x00", 8);
	buf[2] = which;
	
	infectus_sendcommand(buf, 8, 128);
	return 0;
}

/* Get NAND flash chip status */
int infectus_getstatus(void) {
	u8 buf[128];
	int ret,len;
	
	len=infectus_nand_command(buf, 0, NAND_GETSTATUS);
	ret=infectus_sendcommand(buf, len, 128);

	infectus_nand_receive(buf, 1);
	
	return buf[1];
}

/* Wait for NAND flash to be ready */
void wait_flash(void) {
	int status=0;
	while (status != 0xe0) {
		status = infectus_getstatus();
		if(status != 0xe0) printf("Status = %x\n", status);
	}
}

/* Query the first two bytes of the NAND flash chip ID.
   Eventually, this should be used to select the appropriate parameters
   to be used when talking to this chip. */

int infectus_getflashid(void) {
	u8 buf[128];
	int ret,len;

	len=infectus_nand_command(buf, 0, NAND_RESET);
	ret=infectus_sendcommand(buf, len, 128);

	len=infectus_nand_command(buf, 1, NAND_CHIPID, 0);
	ret=infectus_sendcommand(buf, len, 128);

	infectus_nand_receive(buf, 2);
	
	return buf[1] << 8 | buf[2];
}

/* Erase a block of flash memory. */
int infectus_eraseblock(unsigned int blockno) {
	u8 buf[128];
	unsigned int pageno = blockno * pages_per_block;
	int ret, len;

	if (test_mode) return 0;
	
	len=infectus_nand_command(buf, 3, NAND_ERASE_PRE, pageno, pageno >> 8, pageno >> 16);
	ret = infectus_sendcommand(buf, len, 128);
	if (ret!=1) printf("Erase command returned %d\n", ret);

	len=infectus_nand_command(buf, 0, NAND_ERASE_POST);
	ret = infectus_sendcommand(buf, len, 128);
	
	if (check_status) wait_flash();
	return ret;
}


int infectus_readflashpage(u8 *dstbuf, unsigned int pageno) {
	u8 buf[128];
	u8 flash_buf[PAGEBUF_SIZE];
	int ret, len, subpage;
	
	len=infectus_nand_command(buf, 5, NAND_READ_PRE, 0, 
		0, pageno, pageno >> 8, pageno >> 16);
	ret = infectus_sendcommand(buf, len, 128);

	len=infectus_nand_command(buf, 0, NAND_READ_POST);
	ret = infectus_sendcommand(buf, len, 128);
	
	len = 0;
	for(subpage = 0; subpage < ceil((float)(page_size + spare_size) / subpage_size); subpage++) {
		ret = infectus_nand_receive(flash_buf, subpage_size);
		if (ret!= (subpage_size+1)) printf("Readpage returned %d\n", ret);
		memcpy(dstbuf + subpage*subpage_size, flash_buf+1, subpage_size);
		len += ret-1;
	}
	return len;
}

int file_readflashpage(FILE *fp, u8 *dstbuf, unsigned int pageno) {
	fseeko(fp, pageno * (page_size + spare_size), SEEK_SET);
	return fread(dstbuf, 1, page_size + spare_size, fp);
}

int file_writeflashpage(FILE *fp, u8 *dstbuf, unsigned int pageno) {
	fseeko(fp, pageno * (page_size + spare_size), SEEK_SET);
	return fwrite(dstbuf, 1, (page_size + spare_size), fp);
}

int mem_compare(u8 *buf1, u8 *buf2, int size) {
	int i;
	for(i=0; i<size; i++) if(buf1[i] != buf2[i]) break;
	return i;
}

int flash_compare(FILE *fp, unsigned int pageno) {
	u8 buf1[PAGEBUF_SIZE], buf2[PAGEBUF_SIZE];
//	u8 buf3[PAGEBUF_SIZE];
	int x;
	file_readflashpage(fp, buf1, pageno);
	if (check_ecc(buf1)==ECC_WRONG) {
		printf("warning, invalid ECC on disk for page %d\n", pageno);
	}

	infectus_readflashpage(buf2, pageno);
	if (check_ecc(buf2)==ECC_WRONG) {
		printf("warning, invalid ECC in flash for page %d\n", pageno);
	}

	if((x = memcmp(buf1, buf2, page_size + spare_size))) {
//		printf("miscompare on page %d: \n", pageno);
//		infectus_readflashpage(buf3, pageno);
//		if(memcmp(buf2, buf3, sizeof buf3)) {
//			printf("chip is on crack\n");
//		}
	}
	return x;
}

int flash_isFF(u8 *buf, int len) {
	unsigned int *p = (unsigned int *)buf;
	int i;
	len/=4;
	for(i=0;i<len;i++) 	if(p[i]!=0xFFFFFFFF) return 0;
	return 1;
}

int infectus_writeflashpage(u8 *dstbuf, unsigned int pageno) {
	u8 buf[128];
	int ret, len, subpage;
	
	if (test_mode) return 0;
	
	for(subpage = 0; subpage < ceil((float)(page_size + spare_size)/subpage_size); subpage++) {
			len=infectus_nand_command(buf, 5, NAND_WRITE_PRE, subpage * subpage_size,
				(subpage * subpage_size) >> 8 , pageno, pageno >> 8, pageno >> 16);
			ret = infectus_sendcommand(buf, len, 128);

			infectus_nand_send(dstbuf + subpage * subpage_size, subpage_size);

  			len=infectus_nand_command(buf, 0, NAND_WRITE_POST);
			ret = infectus_sendcommand(buf, len, 128);

		if (check_status) wait_flash();
		}
	return 0;
}

int flash_program_block(FILE *fp, unsigned int blockno) {
	u8 buf[PAGEBUF_SIZE];
	unsigned long long usec;
	int pageno, p, miscompares=0;
	printf("\r                                                                     ");
	printf("\r%04x", blockno); fflush(stdout);
	timer_start();
	for(pageno = run_fast?2:0; pageno < pages_per_block; pageno += (run_fast?0x4:1)) {
		p = blockno*pages_per_block + pageno;
		if (flash_compare(fp, p)) {
			putchar('x');
			miscompares++;
// 			if (run_fast) break;   I can't think of a reason not to do this, so ...
			break;
			} else putchar('=');
		fflush(stdout);
	}
	usec = timer_end();
	float rate = (float)blocks_done / (time(NULL) - start_time);
	int secs_remaining = (num_blocks - blockno) / rate;
	if (blocks_done > 2) {
		printf ("%04.1f%% ",blockno * 100.0 / num_blocks);
		if (secs_remaining > 180) {
			printf("%dm\r", secs_remaining/60);
		} else {
			printf("%ds\r", secs_remaining);
		}
	} else putchar('\r');
	if (debug_mode) fprintf(stderr, "Read(%.3f)", usec / 1000000.0f);
	putchar('\r');
	if (miscompares > 0) {
//		printf("   %d miscompares in block\n", miscompares);
		printf("Erasing...");
		infectus_eraseblock(blockno);
		printf("\nProg: ");
		timer_start();
		for(pageno = 0; pageno < pages_per_block; pageno++) {
			p = blockno*pages_per_block + pageno;
			if (file_readflashpage(fp, buf, p)==(page_size + spare_size)) {
				if(flash_isFF(buf, (page_size + spare_size))) {
					putchar('F');
					continue;
				}
				infectus_writeflashpage(buf, p);
				if (verify_after_write) {
					if (flash_compare(fp, p)) {
						putchar('!');
					} else putchar('.');
					fflush(stdout);
				}
			}
		}
		usec = timer_end();
		if (debug_mode) fprintf(stderr,"Write(%.3f)", usec / 1000000.0f);
		putchar('\r');

	}
	blocks_done++;
	return 0;
}

int flash_dump_block(FILE *fp, unsigned int blockno) {
	u8 buf[PAGEBUF_SIZE];
	int pageno, p, ret;
	printf("\r                                                                     ");
	printf("\r%04x", blockno); fflush(stdout);

	for(pageno = 0; pageno < pages_per_block; pageno++) {
		p = blockno*pages_per_block + pageno;
		ret = infectus_readflashpage(buf, p);
		if (ret==(page_size + spare_size)) {
/*			if (check_ecc(buf)==ECC_WRONG) {
				printf("warning, invalid ECC for page %d\n", pageno);
			} */
			file_writeflashpage(fp, buf, p);
			putchar('.');
			fflush(stdout);
		} else {
			printf("error, short read: %d < %d\n", ret, page_size + spare_size);
		}
	}
	float rate = (float)blocks_done / (time(NULL) - start_time);
	int secs_remaining = (num_blocks - blockno) / rate;
	if (blocks_done > 2) {
		printf ("%04.1f%% ",blockno * 100.0 / num_blocks);
		if (secs_remaining > 180) {
			printf("%dm\r", secs_remaining/60);
		} else {
			printf("%ds\r", secs_remaining);
		}
	} else putchar('\r');
	fflush(stdout);
	blocks_done++;
	return 0;
}

void usage(void) {
	fprintf(stderr, "Usage: %s command -[tvwdf] [-b blocksize] filename\n", progname);
	fprintf(stderr, "          -t            test mode -- do not erase or write\n");
	fprintf(stderr, "          -v            verify every byte of written data\n");
	fprintf(stderr, "          -w            wait for status after programming\n");
	fprintf(stderr, "          -x {0,1}      on a dual NAND programmer, choose chip\n");
	fprintf(stderr, "          -f            force: ignore safety checks. Dangerous!\n");
	fprintf(stderr, "          -d            debug (enable debugging output)\n");
	fprintf(stderr, "          -b blocksize  set blocksize; see docs for more info.  Default: 0x%x\n", subpage_size);
	fprintf(stderr, "          -s blockno    start block -- skip this number of blocks\n");
	fprintf(stderr, "                        before proceeding\n");
	fprintf(stderr, "\nValid commands are:\n");
	fprintf(stderr, "         check        check ECC data in file\n");
	fprintf(stderr, "         strip        strip ECC data from file\n");
	fprintf(stderr, "         sums         calculate simple checksum for each page of a file\n");
	fprintf(stderr, "         dump         read from flash chip and dump to file\n");
	fprintf(stderr, "         program      compare file to flash contents, reprogram flash\n");
	fprintf(stderr, "                        to match file\n");
	fprintf(stderr, "         erase        erase the entire flash chip\n");

	exit(1);	
}

int strip_file_ecc(char *filename) {
	u32 pageno;
	if (!filename) {
		fprintf(stderr, "Error: you must specify a filename to strip\n");
		usage();
	}

	char *output_filename=malloc(strlen(filename)+5);
	sprintf(output_filename, "%s.raw", filename);
	
	FILE *fp = fopen(filename, "rb");
	if(!fp) {
		perror("Couldn't open input file: ");
		exit(1);
	}
	fseek(fp, 0, SEEK_END);
	off_t file_length = ftello(fp);
	fseek(fp, 0, SEEK_SET);
	
	if ((file_length % (page_size + spare_size)) && !force) {
		printf("Error: File length is not a multiple of %d bytes.  Are you sure\n",
			page_size + spare_size);
		printf("you want to do this?  Pass -f to force.\n");
		exit(1);
	}

	printf("Stripping ECC data from %s into %s\n", filename, output_filename);
	FILE *fp_out = fopen(output_filename, "wb");
	if(!fp_out) {
		perror("Couldn't open output file: ");
		exit(1);
	}
	
	u64 num_pages = file_length / (page_size + spare_size);
	printf("File size: %"PRIu64" bytes / %"PRIu64" pages / %"PRIu64" blocks\n", 
		file_length, num_pages, num_pages / pages_per_block);
	
	for (pageno=0; (pageno < num_pages) && !feof(fp); pageno++) {
		u8 buf[PAGEBUF_SIZE];
		if ((pageno % 2048)==0) {
			printf ("\r%04.1f%%  ", pageno * 100.0 / num_pages);
			draw_spin();
		}
		fread(buf, 1, page_size + spare_size, fp);
		fwrite(buf, 1, page_size, fp_out);
	}
	fclose(fp_out);
	fclose(fp);
	return 0;
}

int check_file_ecc(char *filename) {
	u32 pageno;
	u32 count_invalid=0, count_wrong=0, count_blank=0, count_ok=0;
	
	if (!filename) {
		fprintf(stderr, "Error: you must specify a filename to check\n");
		usage();
	}
	printf("Checking ECC for file %s\n", filename);
	start_time = time(NULL);
	FILE *fp = fopen(filename, "rb");
	if(!fp) {
		perror("Couldn't open file: ");
		exit(1);
	}
	fseek(fp, 0, SEEK_END);
	off_t file_length = ftello(fp);
	fseek(fp, 0, SEEK_SET);
	u64 num_pages = file_length / (page_size + spare_size);
	printf("File size: %"PRIu64" bytes / %"PRIu64" pages / %"PRIu64" blocks\n", 
		file_length, num_pages, num_pages / pages_per_block);
	for (pageno = 0; pageno < num_pages && !feof(fp); pageno++) {
		u8 buf[PAGEBUF_SIZE];
		file_readflashpage(fp, buf, pageno);
		if ((pageno % 2048)==0) {
			printf ("\r%04.1f%%  ", pageno * 100.0 / num_pages);
			draw_spin();
		}
		switch (check_ecc(buf)) {
			case ECC_OK: 
				count_ok++;
			break;
			case ECC_WRONG:
				count_wrong++;
			 	printf("%d: ecc WRONG\n", pageno);
				printf("Stored ECC: "); hexdump(buf+page_size+48, 16);
				printf("Calc   ECC: "); hexdump(calc_page_ecc(buf), 16);
				break;
			case ECC_INVALID: 
				count_invalid++;
			break;
			case ECC_BLANK: 
				count_blank++;
			break;
			default: break;
		}
	}
	fclose(fp);
	printf("\nTotals: %u pages OK, %u pages WRONG, %u pages blank, %u pages unreadable\n",
		count_ok, count_wrong, count_blank, count_invalid);
	exit(0);
	return 1;
}

/* Precomputed bitcount uses a precomputed array that stores the number of ones
   in each char. */
static int bits_in_char [256] ;

/* Iterated bitcount iterates over each bit. The while condition sometimes helps
   terminates the loop earlier */
int iterated_bitcount (unsigned int n)
{
    int count=0;    
    while (n)
    {
        count += n & 0x1u ;    
        n >>= 1 ;
    }
    return count ;
}
void compute_bits_in_char (void)
{
    unsigned int i ;    
    for (i = 0; i < 256; i++)
        bits_in_char [i] = iterated_bitcount (i) ;
    return ;
}

int generate_checksums(char *filename) {
	u32 pageno;
	char output_filename[1024];
	compute_bits_in_char();
	
	if (!filename) {
		fprintf(stderr, "Error: you must specify a filename to check\n");
		usage();
	}
	sprintf(output_filename, "%s.out", filename);
	printf("Generating sums for file %s, outputting to %s\n", filename, output_filename);
	start_time = time(NULL);
	FILE *fp = fopen(filename, "rb");
	if(!fp) {
		perror("Couldn't open file: ");
		exit(1);
	}
	fseek(fp, 0, SEEK_END);
	off_t file_length = ftello(fp);
	fseek(fp, 0, SEEK_SET);
	u64 num_pages = file_length / (page_size + spare_size);
	printf("File size: %"PRIu64" bytes / %"PRIu64" pages / %"PRIu64" blocks\n", 
		file_length, num_pages, num_pages / pages_per_block);
		
	FILE *out_fp = fopen(output_filename, "w");
	if(!fp) {
		perror("Couldn't open output file: ");
		exit(1);
	}	
	for (pageno = 0; pageno < num_pages && !feof(fp); pageno++) {
		u8 buf[PAGEBUF_SIZE];
		int i;
		unsigned int sum=0;
		file_readflashpage(fp, buf, pageno);
		for (i=0; i<page_size; i++) sum += bits_in_char[buf[i]];
		fprintf(out_fp, "%x %x\n", pageno, sum);
		if ((pageno % 2048)==0) {
			printf ("\r%04.1f%%  ", pageno * 100.0 / num_pages);
			draw_spin();
		}
		
	}
	fclose(fp);
	fclose(out_fp);
	exit(0);
	return 1;
}


void usb_exit_handler(void) {
	usb_close(h);
}

int check_file_validity(FILE *fp) {
	u64 original_offset;
	u64 file_size;
	u8 header_magic[4];
	
	original_offset = ftello(fp);
	fseeko(fp, 0, SEEK_END);
	file_size = ftello(fp);
	
	if (file_size % (page_size + spare_size)) {
		printf("WARNING:  This file does not seem to be a valid dump file,\n");
		printf("          because its filesize (%"PRIu64") is not a multiple of %d\n", 
			file_size, page_size + spare_size);
	}
	
	fseeko(fp, 0, SEEK_SET);
	fread(header_magic, 1, 4, fp);
	
	if (memcmp(header_magic, "\x27\xAE\x8C\x9C", 4)) {
		printf("WARNING: This file does not seem to be a Wii firmware dump.\n");
	}
	fseeko(fp, original_offset, SEEK_SET);
	return 0;
}

int main (int argc,char **argv)
{
	int retval;
	char ch;
	char *filename = NULL;
	
	progname = argv[0];
	printf("amoxiflash version %s, (c) 2008,2009 bushing\n", VERSION);
	
	if (argc < 2) usage();
	char *command = argv[1];
	optind = 2; // skip over command
	
	while ((ch = getopt(argc, argv, "b:tvwx:df:s:q")) != -1) {
		switch (ch) {
			case 'b': subpage_size = strtol(optarg, NULL, 0); break;
			case 't': test_mode = 1; break;
			case 'v': verify_after_write = 1; break;
			case 'w': check_status = 1; break;
			case 'x': chip_select = strtol(optarg, NULL, 0); 
				if (chip_select != 0 && chip_select != 1) {
					fprintf(stderr, "Invalid chip number -- must be 0 or 1\n");
					usage();
				}
				break;
			case 'd': debug_mode = 1; break;
			case 'f': force = 1; break;
			case 's': start_block = strtol(optarg, NULL, 0); break;
			case 'q': quick_check = 1; break;
            case '?':
            default:
                usage();
         }
	}
	argc -= optind;
	argv += optind;
	if (argc > 0) filename = argv[0];

	if (debug_mode) {
		printf("command = %s\n", command);
		printf("subpage_size = %x\n", subpage_size);
		printf("test_mode = %x\n", test_mode);
		printf("verify_after_write = %x\n", verify_after_write);
		printf("check_status = %x\n", check_status);
		printf("chip_select = %x\n", chip_select);
		printf("debug_mode = %x\n", debug_mode);
		printf("force = %x\n", force);
		printf("start_block = %x\n", start_block);
		printf("quick_check = %x\n", quick_check);
		printf("filename = %s\n", filename);
	}

	if (!strcmp(command, "check")) {
		if (!filename) {
			fprintf(stderr, "Error: check requires a filename\n");
			usage();
		}

		retval = check_file_ecc(filename);
		exit(retval);
	}

	if (!strcmp(command, "strip")) {
		if (!filename) {
			fprintf(stderr, "Error: strip requires a filename\n");
			usage();
		}

		retval = strip_file_ecc(filename);
		exit(retval);
	}
	
	if (!strcmp(command, "sums")) {
		if (!filename) {
			fprintf(stderr, "Error: sums requires a filename\n");
			usage();
		}

		retval = generate_checksums(filename);
		exit(retval);
	}
	usb_init();
	atexit(usb_exit_handler);
//	usb_set_debug(2);
 	if ((h = locate_infectus())==0) 
	{
		printf("Could not open the infectus device\n");
		exit(1);
	}

	u32 flashid = 0;
	infectus_reset();
	infectus_get_version();
	infectus_get_loader_version();
	infectus_check_pld_id();
	infectus_selectflash(chip_select);
	usleep(1000);
	flashid = infectus_getflashid();
		
//	printf("ID = %x\n", flashid);
	flashid = infectus_getflashid();
		
//	printf("ID = %x\n", flashid);
	flashid = infectus_getflashid();
		
	printf("ID = %x\n", flashid);

// todo: make a proper database of chip types
	switch(flashid) {
		case 0xECF1: printf("Detected K9F1G08X0A 128Mbyte flash\n"); 
			num_blocks = 1024;
			break;
		case 0xADDC: printf("Detected Hynix 512Mbyte flash\n");
			num_blocks = 4096;
			break;
		case 0xECDC: printf("Detected Samsung 512Mbyte flash\n"); ;
			num_blocks = 4096;
			break;	
		case 0x2CDC: printf("Detected Micron 512Mbyte flash\n"); ;
			num_blocks = 4096;
			break;	
		case 0x98DC: printf("Detected Toshiba 512Mbyte flash\n"); ;
			num_blocks = 4096;
			break;
		case 0:
			printf("No flash chip detected; are you sure target device is powered on?\n");
			exit(1);
		default: 
			printf("Unknown flash ID %04x\n", flashid);
			printf("If this is correct, please notify the author.\n");
			exit(1);
	}

	start_time = time(NULL);
	if(!strcmp(command, "program")) {
		int blockno = start_block;

		if (!filename) {
			fprintf(stderr, "Error: you must specify a filename to program\n");
			usage();
		}
		printf("Programming file %s into flash\n", filename);
		FILE *fp = fopen(filename, "rb");
		if(!fp) {
			perror("Couldn't open file: ");
			exit(1);
		}
		check_file_validity(fp);
		fseek(fp, 0, SEEK_END);
		u64 file_length = ftello(fp);
		fseek(fp, 0, SEEK_SET);
		u64 num_pages = file_length / (page_size + spare_size);
		if (num_pages < (num_blocks * pages_per_block)) {
			fprintf(stderr, "WARNING: File is too short; file is %u blocks, chip is %u blocks\n",
				(u32)num_pages, num_blocks * pages_per_block);
			num_blocks = num_pages / pages_per_block;			
		}
		if (num_pages > (num_blocks * pages_per_block)) {
			fprintf(stderr, "WARNING: File is too long; file is %u blocks, chip is %u blocks\n",
				(u32)num_pages, num_blocks * pages_per_block);
		}

		printf("File size: %"PRIu64" bytes / %"PRIu64" pages / %"PRIu64" blocks\n", 
			file_length, num_pages, num_pages / pages_per_block);
		for (; blockno < num_blocks; blockno++) {
			flash_program_block(fp, blockno);
		}
		fclose(fp);
		exit(0);
	}

	if(!strcmp(command, "dump")) {
		u64 length, offset;
		u32 blockno;

		length = num_blocks * pages_per_block;
		offset = start_block * pages_per_block * (page_size + spare_size);
		printf("Dumping flash @ 0x%"PRIx64" (0x%"PRIx64" bytes) into %s\n", 
				offset, length-offset, filename);

		FILE *fp = fopen(filename, "wb");
		if(!fp) {
			perror("Couldn't open file for writing: ");
			exit(1);
		}
		for(blockno = start_block; blockno < num_blocks; blockno++) {
//			printf("\rDumping block %x", blockno); fflush(stdout);
			flash_dump_block(fp, blockno);
		}
		printf("Done!\n");
		fclose(fp);
		exit(0);
	}

	if(!strcmp(command, "erase")) {
	  int blockno;
	  printf("Erasing %d blocks\n", num_blocks);
	  for (blockno=0; blockno < num_blocks; blockno++) 
	    infectus_eraseblock(blockno);
	  printf("Done!\n");
	  exit(0);
	}
#if 0
		if(!strcmp(argv[argno], "write")) {
			unsigned long long begin, length, offset;
			char *filename;
			argno++;
			filename = argv[argno++];
			begin = strtoll(argv[argno++], NULL, 0);
			length = strtoll(argv[argno++], NULL, 0);

			printf("Programming flash @ 0x%llx (0x%llx bytes) from %s\n", 
				begin, length, filename);
			FILE *fp = fopen(filename, "r");
			if(!fp) {
				perror("Couldn't open file for writing: ");
				break;
			}
			for(offset=0; (offset < length) && !feof(fp); offset+= 0x840) {
				printf("\rwriting page 0x%x", (unsigned int)(begin+offset)/0x840); fflush(stdout);
				
				fread(buf, 1, 0x840, fp);
				infectus_writeflashpage(buf, (begin+offset)/0x840);
			}
			printf("Wrote %llu bytes to %s\n", offset, filename);
			fclose(fp);
			continue;
		}	
#endif
	printf("Unknown command '%s'\n", command);
	usage();
	exit(1);  // not reached
}	

usb_dev_handle *locate_infectus(void) 
{
	unsigned char located = 0;
	struct usb_bus *bus;
	struct usb_device *dev;
	usb_dev_handle *device_handle = 0;
 		
	usb_find_busses();
	usb_find_devices();
 
 	for (bus = usb_busses; bus; bus = bus->next) {
		for (dev = bus->devices; dev; dev = dev->next) {
//			printf ("idVendor=%hx\n", dev->descriptor.idVendor);
			if (dev->descriptor.idVendor == 0x10c4) {	
				located++;
				device_handle = usb_open(dev);
				printf("infectus Device Found @ Address %s \n", dev->filename);
				printf("infectus Vendor ID 0x0%x\n",dev->descriptor.idVendor);
				printf("infectus Product ID 0x0%x\n",dev->descriptor.idProduct);
			}
//			else printf("** usb device %s found **\n", dev->filename);			
		}	
  }

  if (device_handle==0) return (0);
  else return (device_handle);  	
}

   

