
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "altera_up_avalon_audio.h"
#include "fatfs.h"
#include "diskio.h"
#include "monitor.h"
#include <unistd.h>
#include "system.h"
#include "altera_avalon_pio_regs.h"
#include "sys/alt_irq.h"

#define BUFFER_SIZE 512
#define WAV_HEADER_SIZE 44
#define BYTES_PER_SAMPLE 4
#define WORDS_PER_SAMPLE 1
#define MAX_NUM_FILES 20
#define FILE_NAME_LENGTH 20
#define LCD_BUFFER_SIZE 30
#define ESC 27
#define CLEAR_LCD_STRING "[2J"
#define FILE_EMPTY_TOLERANCE 2
#define REVERSE_BUTTON_MASK (1 << 3)
#define STOP_BUTTON_MASK (1 << 2)
#define PLAY_PAUSE_BUTTON_MASK (1 << 1)
#define FORWARD_BUTTON_MASK (1 << 0)
#define DEBOUNCE_THRESHOLD 140
#define DISABLE_THRESHOLD 5
#define REVERSE_STEP 8192
#define BUFFERS_PER_REVERSE 4

FATFS Fatfs[_VOLUMES]; /* File system object for each logical drive */
FIL File1; /* File objects */
DIR Dir; /* Directory object */
FATFS *fs; /* Pointer to file system object */
FILINFO Finfo;
FILE *lcd;
//Used for audio record/playback
alt_up_audio_dev* audio_dev;
unsigned int l_buf;
unsigned int r_buf;
uint8_t audio_buf[BUFFER_SIZE];
char filename[MAX_NUM_FILES][FILE_NAME_LENGTH];
unsigned long file_size[MAX_NUM_FILES];
int song_index = 0;
uint8_t buttons_1, buttons_2, isr_count = 0;
unsigned int debounce_counter = 0, disable_counter = 0, num_files = 0;

enum state {
	START,
	PLAY,
	PAUSE,
	STOP,
	SKIP_FORWARD,
	SKIP_REVERSE,
	FAST_FORWARD,
	FAST_REVERSE,
	SKIP_TO_END
} status = START, next_status = PAUSE;

uint8_t isWav(char* filename) {
	int i = 0;
	while ((filename[i] != '.') && (filename[i] != '\0')) {
		i++;
	}
	return !(strcmp(filename + i, ".wav") && strcmp(filename + i, ".WAV"));
}

void put_rc(FRESULT rc) {
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

void di(uint8_t drv) {
	xprintf("rc=%d\n", (uint16_t) disk_initialize(drv));
}

void fi(uint8_t drv) {
	put_rc(f_mount(drv, &Fatfs[drv]));
}

void fo(char* file_name, uint8_t mode) {
	f_open(&File1, file_name, mode);
}

void fc() {
	put_rc(f_close(&File1));
}

void fl() {
	uint8_t res;
	uint32_t s1 = 0, s2 = 0, i;
	long p1 = 0;
	char null_char = 0;
	res = f_opendir(&Dir, &null_char);
	if (res) {
		put_rc(res);
		return;
	}

	for (;;) {
		res = f_readdir(&Dir, &Finfo);
		if ((res != FR_OK) || !Finfo.fname[0])
			break;
		if (Finfo.fattrib & AM_DIR) {
			s2++;
		} else {
			s1++;
			p1 += Finfo.fsize;
		}
		if (isWav(Finfo.fname)) {
			file_size[s1 - 1] = Finfo.fsize;
			for (i = 0; (i < FILE_NAME_LENGTH) && (Finfo.fname[i] != '/0');
					i++) {
				filename[s1 - 1][i] = Finfo.fname[i];
			}
			xprintf("%c%c%c%c%c %u/%02u/%02u %02u:%02u %9lu  %s\n",
					(Finfo.fattrib & AM_DIR) ? 'D' : '-',
					(Finfo.fattrib & AM_RDO) ? 'R' : '-',
					(Finfo.fattrib & AM_HID) ? 'H' : '-',
					(Finfo.fattrib & AM_SYS) ? 'S' : '-',
					(Finfo.fattrib & AM_ARC) ? 'A' : '-',
					(Finfo.fdate >> 9) + 1980, (Finfo.fdate >> 5) & 15,
					Finfo.fdate & 31, (Finfo.ftime >> 11),
					(Finfo.ftime >> 5) & 63, file_size[s1 - 1],
					&(filename[s1 - 1][0]));
		} else {
			s1--;
		}
	}
	xprintf("%4u File(s),%10lu bytes total\n%4u Dir(s)", s1, p1, s2);
	res = f_getfree(&null_char, (uint32_t *) &p1, &fs);
	if (res == FR_OK)
		xprintf(", %10lu bytes free\n", p1 * fs->csize * 512);
	else
		put_rc(res);
	num_files = s1;
}

long fp(long bytes_to_play, int speed) {
	uint32_t s1, i;
	uint16_t* buf_ptr;
	long total_bytes_played = 0;
	//file_bytes is the number of bytes to read from the file
	while (total_bytes_played < bytes_to_play) {
		f_read(&File1, audio_buf, BUFFER_SIZE, &s1);
		if (s1 == 0) {
			break;
		}
		for (i = 0; i <= (s1 - (speed * BYTES_PER_SAMPLE));
				i += speed * BYTES_PER_SAMPLE) {
			buf_ptr = (uint16_t*) (audio_buf + i);
			r_buf = *buf_ptr;
			buf_ptr++;
			l_buf = *buf_ptr;

			while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_RIGHT)
					< WORDS_PER_SAMPLE)
				;
			alt_up_audio_write_fifo(audio_dev, &(r_buf), WORDS_PER_SAMPLE,
			ALT_UP_AUDIO_RIGHT);
			while (alt_up_audio_write_fifo_space(audio_dev, ALT_UP_AUDIO_LEFT)
					< WORDS_PER_SAMPLE)
				;
			alt_up_audio_write_fifo(audio_dev, &(l_buf), WORDS_PER_SAMPLE,
			ALT_UP_AUDIO_LEFT);

			total_bytes_played += speed * BYTES_PER_SAMPLE;
		}
	}
	return total_bytes_played;
}

int fp_remove_header() {
	uint32_t s1;
	// Remove .wav file header
	f_read(&File1, audio_buf, WAV_HEADER_SIZE, &s1);
	return s1;
}

void lcd_print(char* message) {
	if (lcd != NULL) {
		fprintf(lcd, message);
	} else {
		alt_printf("Error: could not open LCD display\n");
	}
}

void lcd_clear() {
	if (lcd != NULL) {
		fprintf(lcd, "%c%s", ESC, CLEAR_LCD_STRING);
	} else {
		alt_printf("Error: could not open LCD display\n");
	}
}

void lcd_open() {
	lcd = fopen("/dev/lcd_display", "w");
	if (lcd == NULL)
		alt_printf("Error: could not open LCD display\n");
}

void lcd_close() {
	fclose(lcd);
}

void increment_song_index() {
	song_index++;
	song_index %= num_files;
}

void decrement_song_index() {
	if (song_index == 0) {
		song_index = num_files - 1;
	} else {
		song_index--;
	}
}

//Interrupt service routine for checking the buttons
static void button_isr(void* context, alt_u32 id) {
	IOWR(BUTTON_PIO_BASE, 2, 0x00); //Disable buttons interrupt
	if (disable_counter >= DISABLE_THRESHOLD) {
		buttons_1 = IORD(BUTTON_PIO_BASE, 0);
		debounce_counter = 0;
		while (debounce_counter < DEBOUNCE_THRESHOLD) {
			buttons_2 = IORD(BUTTON_PIO_BASE, 0);
			if (buttons_1 != buttons_2) {
				buttons_1 = buttons_2;
				debounce_counter = 0;
			} else {
				debounce_counter++;
			}
		}
		isr_count++;
		IOWR(LED_PIO_BASE, 0, isr_count);
		buttons_1 = ~buttons_1;
		if (buttons_1 & REVERSE_BUTTON_MASK) {
			if ((status == PAUSE) || (status == STOP)) {
				status = SKIP_REVERSE;
			} else if (status == PLAY) {
				status = FAST_REVERSE;
			} else if (next_status == PAUSE) {
				next_status = SKIP_REVERSE;
			} else if (next_status == PLAY) {
				next_status = FAST_REVERSE;
			}
		} else if (status == FAST_REVERSE) {
			status = PLAY;
		}
		if (buttons_1 & FORWARD_BUTTON_MASK) {
			if ((status == PAUSE) || (status == STOP)) {
				status = SKIP_FORWARD;
			} else if (status == PLAY) {
				status = FAST_FORWARD;
			} else if (next_status == PAUSE) {
				next_status = SKIP_FORWARD;
			} else if (next_status == PLAY) {
				next_status = FAST_FORWARD;
			}
		} else if (status == FAST_FORWARD) {
			status = PLAY;
		}
		if (buttons_1 & STOP_BUTTON_MASK) {
			status = STOP;
		}
		if (buttons_1 & PLAY_PAUSE_BUTTON_MASK) {
			if (status == PLAY) {
				status = PAUSE;
			} else {
				status = PLAY;
			}
		}
	}
	IOWR(BUTTON_PIO_BASE, 2, 0x0F); //Enable buttons interrupt
	IOWR(BUTTON_PIO_BASE, 3, 0x0); //Clear interrupt
}

int main() {
	long file_bytes, curr_position;
	char lcd_buffer[LCD_BUFFER_SIZE];
	//Open the LCD
	lcd_open();
	//Open the audio port
	audio_dev = alt_up_audio_open_dev("/dev/Audio");
	if (audio_dev == NULL)
		alt_printf("Error: could not open audio device\n");
	else
		alt_printf("Opened audio device\n");
	di(0);
	fi(0);
	fl();

	IOWR(LED_PIO_BASE, 0, 0x0);

	IOWR(BUTTON_PIO_BASE, 2, 0x0F); //Enable buttons interrupt
	alt_irq_register(BUTTON_PIO_IRQ, (void*) 0, button_isr); //Register buttons interrupt

	while (1) {
		switch (status) {
		case START:
			file_bytes = file_size[song_index];
			fo(filename[song_index], 1);
			file_bytes -= fp_remove_header();
			sprintf(lcd_buffer, "%d. %s", song_index + 1, filename[song_index]);
			lcd_open();
			lcd_clear();
			lcd_print(lcd_buffer);
			lcd_close();
			status = next_status;
			break;
		case PLAY:
			file_bytes -= fp(BUFFER_SIZE, 1);
			if (file_bytes <= FILE_EMPTY_TOLERANCE) {
				increment_song_index();
				next_status = PLAY;
				status = START;
			}
			break;
		case PAUSE:
			break;
		case STOP:
			file_bytes = file_size[song_index];
			fo(filename[song_index], 1);
			file_bytes -= fp_remove_header();
			status = PAUSE;
			break;
		case SKIP_FORWARD:
			increment_song_index();
			status = START;
			next_status = PAUSE;
			break;
		case SKIP_REVERSE:
			decrement_song_index();
			status = START;
			next_status = PAUSE;
			break;
		case FAST_FORWARD:
			file_bytes -= fp(BUFFER_SIZE, 2);
			if (file_bytes <= FILE_EMPTY_TOLERANCE) {
				increment_song_index();
				next_status = FAST_FORWARD;
				status = START;
			}
			break;
		case FAST_REVERSE:
			curr_position = file_size[song_index] - file_bytes;
			if (curr_position > REVERSE_STEP) {
				f_lseek(&File1, curr_position - REVERSE_STEP);
				file_bytes += REVERSE_STEP;
				file_bytes -= fp(BUFFERS_PER_REVERSE * BUFFER_SIZE, 1);
			} else {
				decrement_song_index();
				next_status = SKIP_TO_END;
				status = START;
			}
			break;
		case SKIP_TO_END:
			f_lseek(&File1, file_size[song_index]);
			file_bytes = 0;
			status = FAST_REVERSE;
			break;
		}
		if (disable_counter < DISABLE_THRESHOLD) {
			disable_counter++;
		}
	}

	return 0;
}
