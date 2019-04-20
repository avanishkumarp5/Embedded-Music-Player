
#define __MAIN_C__

/*=========================================================================*/
/*  Includes                                                               */
/*=========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <system.h>
#include <sys/alt_alarm.h>
#include <io.h>

#include "fatfs.h"
#include "diskio.h"

#include "ff.h"
#include "monitor.h"
#include "uart.h"

#include "alt_types.h"
#include "sys/alt_irq.h"

#include <altera_up_avalon_audio.h>
#include <altera_up_avalon_audio_and_video_config.h>
#include <altera_avalon_lcd_16207_regs.h>


//global variable initializations
FILINFO Finfo;
FIL File1;
DIR Dir; /* Directory object */
FATFS Fatfs[_VOLUMES]; /* File system object for each logical drive */

alt_up_audio_dev * audio_dev;
uint8_t Buff[8192] __attribute__ ((aligned(4))); /* Working buffer */
uint32_t s2=sizeof(Buff);
uint32_t ofs = 0;int i=0,prsd=0;
char *ptr;
volatile int gprsd=0x0,gbslct;
volatile unsigned int g_temp=0, plps=0, stop=0, skip=0;



static void put_rc(FRESULT rc) {//checks the opening of file
	const char *str =
			"OK\0" "DISK_ERR\0" "INT_ERR\0" "NOT_READY\0" "NO_FILE\0" "NO_PATH\0"
					"INVALID_NAME\0" "DENIED\0" "EXIST\0" "INVALID_OBJECT\0" "WRITE_PROTECTED\0"
					"INVALID_DRIVE\0" "NOT_ENABLED\0" "NO_FILE_SYSTEM\0" "MKFS_ABORTED\0" "TIMEOUT\0"
					"LOCKED\0" "NOT_ENOUGH_CORE\0" "TOO_MANY_OPEN_FILES\0";
	FRESULT i;

	for (i = 0; i != rc && *str; i++) {
		while (*str++)
			;
	}
	xprintf("rc=%u FR_%s\n", (uint32_t) rc, str);
}

static void d_init(){//disk initialize
	int p1=0;
	(uint16_t) disk_initialize((uint8_t ) p1);
}

static void f_init(){//initialize new file system object
	put_rc(f_mount((uint8_t) 0, &Fatfs[0]));
}

static void file_open(char *ptr, long p1){//open the request wav file
		put_rc(f_open(&File1, ptr, (uint8_t) p1));
		return;
}

static void writeLCD(char *txt, int num){//write requested characters to lcd
	FILE *lcd;
	lcd = fopen("/dev/lcd_display", "w");
	if (lcd != NULL )
	fprintf(lcd,"%d %s\n\n",num+1, txt );

}

int isWavCode(char *filename){//checks for extension .wav

	char*ptr2 = filename;
	while(*ptr2 !='.')
		ptr2++;
	ptr2++;
	//printf("%c%c%c", *ptr2,*(ptr2+1),*(ptr2+2));
	if(*ptr2=='W'){
		ptr2++;
	}
	else return 0;
	if(*ptr2=='A')
		ptr2++;
	else return 0;
	if(*ptr2=='V')
		return 1;
	else return 0;
}

char songIndex(char file2name[20][16], long fileSize[20]){//returns the filesize, and filename arrays
	int res;
	d_init();
	f_init();
	while (*ptr == ' ')
		ptr++;
	res = f_opendir(&Dir, ptr);
	if (res) {
		put_rc(res);
		return 0;
	}

	int p1=0,  s1=0,s2=0,j=0;
	for (;;) {
		res = f_readdir(&Dir, &Finfo);
		if ((res != FR_OK) || !Finfo.fname[0])
			return 0;
		if (Finfo.fattrib & AM_DIR) {
			s2++;
		} else {
			s1++;
			p1 += Finfo.fsize;
		}

		if(isWavCode(&Finfo.fname[0])){//check if it is an isWave file
			strcpy(file2name[j],Finfo.fname);//Finfo.fname already contains the song name

			fileSize[j]=Finfo.fsize;
			j++;
		}
	}
}


//interrupt handler
static void btn_irq (void* context, alt_u32 id) {
	int btn;
	btn=IORD(BUTTON_PIO_BASE,0);

	if (btn!=0xf && gprsd==0){
		//debounce
		/****************timer start 20ms*************/
					IOWR(TIMER_0_BASE,2,0x4240);
					IOWR(TIMER_0_BASE,3,0xF);
					IOWR(TIMER_0_BASE,1,0x5);
		/****************************************/
		gprsd=0x1;//when first pressed gprsd set to 1
		g_temp=btn^0xf;//store the btn value
		if(plps==1){
			if (g_temp==1)
				skip=1;
				else if(g_temp==8)
				skip=2;
		}
	}

/*****************Case button released************************/
	 if(btn==0xf && gprsd==0x1){
	/****************timer start 20ms*************/
		IOWR(TIMER_0_BASE,2,0x4240);
		IOWR(TIMER_0_BASE,3,0xF);
		IOWR(TIMER_0_BASE,1,0x5);
	/****************************************/
	while(IORD(TIMER_0_BASE,0)==2){
	}//timer over

		gprsd=0x0;
		gbslct=g_temp;
		skip=0;
		if(gbslct==2)	plps=!plps;
		if(gbslct==4)	stop=1;
	}

	//printf("\n skip=%d",skip);
	IOWR(BUTTON_PIO_BASE, 3, 0x0); //stay high until all the tasks are done clear the interrupt, register offset 3 corresponds to edge capture bit
}

static void timer_irq(void* context, alt_u32 id){
	IOWR(TIMER_0_BASE,0,0x0);
}


int main(void){

//********************variable declaration*********************//
	unsigned int p1,play=0,j=0;
	unsigned int pcnt=0,plen=12,fifospace,cnt, res, l_buf, r_buf;
	uint32_t ofs=0;
	char flname[20][16];//filename variable
	long flsize[16];//fileSize

	alt_irq_register(BUTTON_PIO_IRQ, (void *)0, btn_irq);//register the isr for btns
	alt_irq_register(TIMER_0_IRQ, (void *)0, timer_irq);
	IOWR(BUTTON_PIO_BASE, 3, 0xf);
	IOWR(BUTTON_PIO_BASE, 2,0xf); //enable Button IRQ

	IOWR(TIMER_0_BASE,2,0x4240);
	IOWR(TIMER_0_BASE,3,0xF);

	//open audio core
	audio_dev = alt_up_audio_open_dev("/dev/Audio");
	songIndex(flname,flsize);//load up the song names and sizes

	/***************test code****************************/
	for(;;){//print list of names and sizes
		if(isWavCode(&flname[j][0])){
			xprintf("%d %s \n",flsize[j], &flname[j][0]);
			j++;
		}
		else break;
	}
	int listlen=j-1;
	IOWR(BUTTON_PIO_BASE,3,0);
	/********************************************************************/

	/*********************INTERFACE FUNCTIONS****************************/
	short int playnew=0;
	writeLCD(&flname[i][0],i);
	file_open(&flname[i][0],1);

	while(1){
		if(gbslct!=0){
			switch(gbslct){
			case 1:
				i++;
				if (!isWavCode(&flname[i][0]))i=0;
				writeLCD(&flname[i][0],i);
				file_open(&flname[i][0],1);
				gbslct=0;
				playnew=1;//set songflag to new;
			break;

			case 2:
			//initial play call
			if(playnew)	{
				p1=flsize[i];
				playnew=!playnew;
			}
			play=1;
			gbslct=0;
			break;

			case 4:
			file_open(&flname[i][0],1);//set the pointer to the front of track
			stop=0;
			printf("\n stop main=%d", stop);
			plps=0;
			playnew=1;
			gbslct=0;
			break;

			case 8:
			i--;
			if(i<0) i=listlen;
			printf("\n i=%d ",i);
			writeLCD(&flname[i][0],i);
			file_open(&flname[i][0],1);
			gbslct=0;
			playnew=1;//set songflag to new
			break;

			default:
				IOWR(LED_PIO_BASE,0,0);
			}
		}
	/*********************PLAY FUNCTION****************************/
	if(play){
		//printf("p1=%lu,%l \n",p1,full_size);
		ofs = File1.fptr;
		uint32_t prev=ofs;
		ofs+=44;//offset the header size
		res = f_read(&File1, Buff, 44, &s2);//push the header in the buffer first.
		//p1 is the number of bytes to read from the file
		cnt = 0;
		while (p1 > 0 && plps &&!stop) {
			if ((uint32_t) p1 >= plen) {
				cnt = plen;
				if(skip==2){
					p1+=plen;//add plen bytes to p1 to keep increasing size if previous is held
					if(p1>=flsize[i])//if the size reaches max size, return to main
						break;
				}
				else p1 -= plen;
			}
			else{
				cnt = p1;
				p1 = 0;
			}
			if(skip==2) f_lseek(&File1, ofs);//set the file pointer to 32 bytes earlier in the file
			res = f_read(&File1, Buff, cnt, &s2);//read next 'cnt' bytes into the Buffer 'Buff' from 0.

			if (res != FR_OK) {//check if the read was successful
				put_rc(res);
				break;
			}
			if (!cnt)
			break;

			pcnt=0;
			while(pcnt<cnt && plps &&!stop){//loop to play first 64 bytes
				fifospace = alt_up_audio_read_fifo_avail (audio_dev, ALT_UP_AUDIO_RIGHT);
				if (fifospace > 0){ //if space is available,write next 4 bytes to the l and r buffers.
					l_buf= (Buff[pcnt+1]<<8)| (Buff[pcnt]); //combine first two bytes left buffer taking care of endian
					r_buf= (Buff[pcnt+3]<<8)|(Buff[pcnt+2]);//combine next two bytes left buffer taking care of endian

					// write audio buffer
					alt_up_audio_write_fifo (audio_dev, &(l_buf), 1, ALT_UP_AUDIO_LEFT);
					alt_up_audio_write_fifo (audio_dev, &(r_buf), 1, ALT_UP_AUDIO_RIGHT);

					if(skip==1)
						pcnt+=8;
					else if(skip==2)
						pcnt-=16;
					else
						pcnt+=4;
				}
				if(stop) break;//break loop when stopped
				}
				if(skip==2)	ofs -= plen;
				else ofs += plen;//increment playback header pointer to next
			}
	plps=0;//song finished go to pause state
	play=0;
	if(p1<=0) file_open(&flname[i][0],1);

	}
	/*********************PLAY FUNCTIONS****************************/
	}

	/*********************INTERFACE FUNCTIONS****************************/


}

