/*
 *  p2000scanner: A P2000 paging decoder (1.0-BETA)
 *
 *  Based on the source of PDW (POCSAG, FLEX, ACARS, MOBITEX & ERMES Decoder)
 *
 *  Jason Petty (2001-2004) and Peter Hunt (2004-2010)
 *
 *  Modified by Jurgen Brunink and Martin Kollaard (2013)
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "multimon.h"
#include <stdio.h>   	/* Standard input/output definitions */
#include <string.h>  	/* String function definitions */
#include <unistd.h>  	/* UNIX standard function definitions */
#include <fcntl.h>   	/* File control definitions */
#include <errno.h>   	/* Error number definitions */
#include <termios.h> 	/* POSIX terminal control definitions */
#include <stdio.h>      /* printf */
#include <string.h>     /* strcat */
#include <stdlib.h>     /* strtol */
#include <strings.h>
#include <time.h>       /* time_t, struct tm, time, localtime, asctime */
#include <assert.h>
#include <ctype.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/stat.h> /* umask */
#include <stdarg.h>	/* va_list */

#define SYNC1	0xA6C6
#define SYNC2	0xAAAA

#define EOT1	0xAAAA
#define EOT2	0xFFFF

#define BUFSIZE 1024

#define STAT_FLEX1600		2

#define MODE_SECURE			0
#define MODE_SHORT_INSTRUCTION		1
#define MODE_SH_TONE			2
#define MODE_STNUM			3
#define MODE_SFNUM			4
#define MODE_ALPHA			5
#define MODE_BINARY			6
#define MODE_NUNUM			7

#define MAX_STR_LEN			5120

#define MSG_CAPCODE		1
#define MSG_TIME		2
#define MSG_DATE		3
#define MSG_MODE		4
#define MSG_TYPE		5
#define MSG_BITRATE		6
#define MSG_MESSAGE		7
#define MSG_MOBITEX		8
#define MSG_TIMESTAMP		9

#define MAXIMUM_GROUPSIZE       1000
#define CAPCODES_INDEX		0

/* Function prototypes */

int ecd();
void SortGroupCall(int groupbit);
//void writeToLog(char *message);
/* End function prototypes */

char vtype[8][9] = { "SECURE", "INSTR", "SH/TONE", "StNUM", "SfNUM", "ALPHA",
		"BINARY", "NuNUM" };
char aGroupnumbers[16][8] = { "-1", "-2", "-3", "-4", "-5", "-6", "-7", "-8",
		"-9", "10", "11", "12", "13", "14", "15", "16" };

int iMessagesCounter = 0;

char Current_MSG[10][MAX_STR_LEN];			// PH: Buffer for all message items
// 1 = MSG_CAPCODE
// 2 = MSG_TIME
// 3 = MSG_DATE
// 4 = MSG_MODE
// 5 = MSG_TYPE
// 6 = MSG_BITRATE
// 7 = MSG_MESSAGE
// 8 = MSG_MOBITEX

char Previous_MSG[2][10][MAX_STR_LEN];	// PH: Buffer for previous message items
// PH: [8]=last filtered messagetext

int aGroupCodes[17][MAXIMUM_GROUPSIZE];
int GroupFrame[17] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1 };

int FlexTempAddress;			// PH: Set to corresponding groupaddress (0-15)

int iFrameCount = 0;

unsigned char message_buffer[MAX_STR_LEN + 1];	// buffer for message characters

int iCurrentCycle, iCurrentFrame;	// Current flex cycle / frame

long int capcode;

char block[256]; //per phase
long int frame[200]; //per phase

int pd_i = 0;
unsigned int bch[1025], ecs[25];     // error correction sequence

int bFLEX_isGroupMessage = 0;

char ob[32];

int iFlexBlock = 0;
int iFlexBlockCount = 0;
int iFlexTimer = 0;

pthread_t threads[1];

int iMessageIndex = 0;

int syncs[8] = { 0x870C, 0x7B18, 0xB068, 0xDEA0, 0, 0, 0, 0x4C7C };
int g_sps = 1600;
int g_sps2 = 1600;
int level = 2;

double * rcverpointer;
double rcver[65];


int ircver = 0;

double exc = 0.0;

int flex_speed = STAT_FLEX1600;

// this table translates received modem status line combinations into
// the received symbol; it gives the number of modem status lines that
// are high (other than RI) for a given line status nybble
int iLineSymbols[16] = { 0, 1, 1, 2, 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3 };
int iConvertingGroupcall = 0;

int containsUnknownCharacters = 0;

int ecd() {
	int synd, b1, b2, i;
	int errors = 0, parity = 0;

	int ecc = 0x000;
	int acc = 0;

	// run through error detection and correction routine

	for (i = 0; i <= 20; i++) {
		if (ob[i] == 1) {
			ecc = ecc ^ ecs[i];
			parity = parity ^ 0x01;
		}
	}

	for (i = 21; i <= 30; i++) {
		acc = acc << 1;
		if (ob[i] == 1)
			acc = acc ^ 0x01;
	}

	synd = ecc ^ acc;

	if (synd != 0) // if nonzero syndrome we have error
			{
		if (bch[synd] != 0) // check for correctable error
				{
			b1 = bch[synd] & 0x1f;
			b2 = bch[synd] >> 5;
			b2 = b2 & 0x1f;

			if (b2 != 0x1f) {
				ob[b2] = ob[b2] ^ 0x01;
				ecc = ecc ^ ecs[b2];
			}

			if (b1 != 0x1f) {
				ob[b1] = ob[b1] ^ 0x01;
				ecc = ecc ^ ecs[b1];
			}
			errors = bch[synd] >> 12;
		} else
			errors = 3;

		if (errors == 1)
			parity = parity ^ 0x01;
	}

	// check parity ....
	parity = (parity + bit10(ecc)) & 0x01;

	if (parity != ob[31])
		errors++;

	if (errors > 3)
		errors = 3;


	return (errors);
}

void setupecc() {
	unsigned int srr, j, k;
	int i, n;

	// calculate all information needed to implement error correction
	srr = 0x3B4;

	for (i = 0; i <= 20; i++) {
		ecs[i] = srr;
		if ((srr & 0x01) != 0)
			srr = (srr >> 1) ^ 0x3B4;
		else
			srr = srr >> 1;
	}

	// bch holds a syndrome look-up table telling which bits to correct
	// first 5 bits hold location of first error; next 5 bits hold location
	// of second error; bits 12 & 13 tell how many bits are bad
	for (i = 0; i < 1024; i++)
		bch[i] = 0;

	for (n = 0; n <= 20; n++)	// two errors in data
			{
		for (i = 0; i <= 20; i++) {
			j = (i << 5) + n;
			k = ecs[n] ^ ecs[i];
			bch[k] = j + 0x2000;
		}
	}

	// one error in data
	for (n = 0; n <= 20; n++) {
		k = ecs[n];
		j = n + (0x1f << 5);
		bch[k] = j + 0x1000;
	}

	// one error in data and one error in ecc portion
	for (n = 0; n <= 20; n++) {
		for (i = 0; i < 10; i++)  // ecc screwed up bit
				{
			k = ecs[n] ^ (1 << i);
			j = n + (0x1f << 5);
			bch[k] = j + 0x2000;
		}
	}

	// one error in ecc
	for (n = 0; n < 10; n++) {
		k = 1 << n;
		bch[k] = 0x3ff + 0x1000;
	}

	// two errors in ecc
	for (n = 0; n < 10; n++) {
		for (i = 0; i < 10; i++) {
			if (i != n) {
				k = (1 << n) ^ (1 << i);
				bch[k] = 0x3ff + 0x2000;
			}
		}
	}
}

void parseSingleMessage() {
	if (!iConvertingGroupcall) {
		message_buffer[iMessageIndex] = '\0';
		iMessageIndex = 0;
	}
	memcpy(Current_MSG[MSG_MESSAGE], message_buffer, MAX_STR_LEN);
	containsUnknownCharacters = 0;
}



void ConvertGroupcall(int groupbit, char *vtype, int capcode) {
	message_buffer[iMessageIndex] = '\0';

	//char address[16]="";
	int addresses = 0;

	if (capcode >= 2029568 && capcode <= 2029583) {
		if (GroupFrame[groupbit] == iCurrentFrame) {
			for (int nCapcode = 1;
					nCapcode <= aGroupCodes[groupbit][CAPCODES_INDEX];
					nCapcode++) {
				addresses += aGroupCodes[groupbit][nCapcode];
			}
			addresses += aGroupCodes[groupbit][CAPCODES_INDEX] << 25;
			//printf("address: %i\n", addresses);

			iConvertingGroupcall = groupbit + 1;

			//printf("GROUP%s\n", aGroupnumbers[iConvertingGroupcall - 1]);

			strcpy(Current_MSG[MSG_TYPE], "GROUP");
			memcpy(Current_MSG[MSG_MESSAGE], message_buffer, MAX_STR_LEN);

			for (int nCapcode = 1;
					nCapcode <= aGroupCodes[groupbit][CAPCODES_INDEX];
					nCapcode++) {
				if (aGroupCodes[groupbit][nCapcode] == 9999999) {
					strcpy(Current_MSG[MSG_CAPCODE], "???????");
				} else {
					sprintf(Current_MSG[MSG_CAPCODE], "%07i",
							aGroupCodes[groupbit][nCapcode]);
				}
				ShowMessage();
			}

			sprintf(Current_MSG[MSG_CAPCODE], "%07i", capcode);
			strcpy(Current_MSG[MSG_TYPE], vtype);

			containsUnknownCharacters = 0;

			iMessagesCounter++;
			ShowMessage();
			memset(aGroupCodes[groupbit], 0, sizeof(int) * MAXIMUM_GROUPSIZE);

			GroupFrame[groupbit] = -1;
			iConvertingGroupcall = 0;		// PH: Reset for next groupmessage

		}
	}
	iMessageIndex = 0;
}

//void ShowMessage() {
//	if (!iConvertingGroupcall) {
//		message_buffer[iMessageIndex] = '\0';
//		iMessageIndex = 0;
//	}
//
//	memcpy(Current_MSG[MSG_MESSAGE], message_buffer, MAX_STR_LEN);
//
//	printf("Message: %s\n", Current_MSG[MSG_MESSAGE]);
//
//}

void ShowMessage()
{
	verbprintf(0, "BEGIN MSG\n");
	verbprintf(0, "Message: %s\n", Current_MSG[MSG_MESSAGE]);
	verbprintf(0, "Capcode: %s\n", Current_MSG[MSG_CAPCODE]);
	verbprintf(0, "Type: %s\n", Current_MSG[MSG_TYPE]);
	verbprintf(0, "Time: %s\n", Current_MSG[MSG_TIME]);
	verbprintf(0, "Date: %s\n", Current_MSG[MSG_DATE]);
	verbprintf(0, "Timestamp: %s\n", Current_MSG[MSG_TIMESTAMP]);
	verbprintf(0, "END MSG\n");
}

void SortGroupCall(int groupbit)	// PH: Sort aGroupCodes[groupbit]
{
	for (int nCapcode = 1; nCapcode <= aGroupCodes[groupbit][CAPCODES_INDEX];
			nCapcode++) {
		int min, j;

		for (min = nCapcode, j = nCapcode + 1; aGroupCodes[groupbit][j] > 0;
				j++) {
			if (aGroupCodes[groupbit][j] < aGroupCodes[groupbit][min])
				min = j;
		}

		int tmp = aGroupCodes[groupbit][nCapcode];

		aGroupCodes[groupbit][nCapcode] = aGroupCodes[groupbit][min];
		aGroupCodes[groupbit][min] = tmp;  // swap them
	}
}

void AddAssignment(int assignedframe, int groupbit, int capcode) {
	//printf("Assignment capcode: %d\n", capcode);
	if ((GroupFrame[groupbit] != assignedframe) && (GroupFrame[groupbit] != -1)
			&& (aGroupCodes[groupbit][CAPCODES_INDEX])) {
		/*if (groupbit < 16) */
	}

	if (aGroupCodes[groupbit][CAPCODES_INDEX] < MAXIMUM_GROUPSIZE) {
		aGroupCodes[groupbit][CAPCODES_INDEX]++;
		aGroupCodes[groupbit][aGroupCodes[groupbit][CAPCODES_INDEX]] = capcode;

		GroupFrame[groupbit] = assignedframe;
	}
	if (iMessageIndex) {
		message_buffer[iMessageIndex] = 0;		// terminate the buffer string
		iMessageIndex = 0;
	}
}

// checksum check for BIW and vector type words
// returns: 0 if word passes test; 1 if test failed
int xsumchk(long int l) {
	// was word already marked as bad?
	if (l > 0x3fffffl)
		return (1);

	// 4 bit checksum is made by summing up remaining part of word
	// in 4 bit increments, and taking the 4 lsb and complementing them.
	// Therefore: if we add up the whole word in 4 bit chunks the 4 lsb
	// bits had better come out to be 0x0f

	int xs = (int) (l & 0x0f);
	xs += (int) ((l >> 4) & 0x0f);
	xs += (int) ((l >> 8) & 0x0f);
	xs += (int) ((l >> 12) & 0x0f);
	xs += (int) ((l >> 16) & 0x0f);
	xs += (int) ((l >> 20) & 0x01);

	xs = xs & 0x0f;

	if (xs == 0x0f) {
		//CountBiterrors(0);
		return (0);
	} else {
		//CountBiterrors(1);
		return (1);
	}
}

void FlexTIME(struct l2_state_flex *rx) {

	int year, month, day, hour, minutes;

	int i;
	//char temp[MAX_PATH];
	//char szFlexTIME[128];

	//float frame_seconds= (iCurrentFrame & 0x1f) * 1.875;
	//int seconds		 = frame_seconds;

	static int FLEX_time = 0, FLEX_date = 0;
	//static int count = 0;

	int bTime = 0, bDate = 0;

	for (i = 0; i <= ((frame[0] >> 8) & 0x03); i++) {
		if (xsumchk(frame[i]) != 0) {
			//printf("CRC error in BIW[%d]! (0x%08X)\n", i, frame[i]);
			rx->flex_total_error_count++;
			return;
		}
		if (i) {
			switch ((frame[i] >> 4) & 0x07) {
			case 0:
//				printf(
//						(("frame[i]: Type == SSID/Local ID.s (i8-i0)(512) & Coverage Zones (c4-c0)(32)\n")));
				break;
			case 1:
				frame[i] >>= 7;
				year = (frame[i] & 0x1F) + 1994;
				//printf("year: %lu\n", (frame[i] & 0x1F) + 1994);
				frame[i] >>= 5;
				day = frame[i] & 0x1F;
				//recFlexTime.wDay = frame[i] & 0x1F;
				frame[i] >>= 5;
				month = (frame[i] & 0xF);
				//recFlexTime.wMonth = (frame[i] & 0xF);
				bDate = 1;
				FLEX_date = 1;
				//printf((("BIW DATE: %d-%d-%d\n"), recFlexTime.wDay, recFlexTime.wMonth, recFlexTime.wYear));
				break;
			case 2:
				frame[i] >>= 7;
				hour = frame[i] & 0x1F;
				//recFlexTime.wHour = frame[i] & 0x1F;
				frame[i] >>= 5;
				minutes = frame[i] & 0x3F;
				//recFlexTime.wMinute = frame[i] & 0x3F;
				frame[i] >>= 6;
				//seconds = seconds;
				//recFlexTime.wSecond = seconds;
				bTime = 1;
				FLEX_time = 1;
				//printf((("BIW TIME: %02d:%02d:%02d\n"), recFlexTime.wHour, recFlexTime.wMinute, recFlexTime.wSecond));
				break;
			case 5:
//				printf(
//						(("frame[i]: Type == System Information (I9-I0. A3-A0) - related to NID roaming\n")));
				break;
			case 7:
//				printf(
//						(("frame[i]: Type == Country Code & Traffic Management Flags (c9-c0, T3-T0)\n")));
				break;
			case 6:
			case 3:
			case 4:
//				printf((("frame[i]: Type == Reserved\n")));
				break;
			}
		}
	}



}

void show_phase_speed(int vt) {
	/*
	 int v;

	 switch(flex_speed)
	 {
	 default:
	 case STAT_FLEX1600:
	 v = 1600;
	 break;
	 }
	 */

	if (vt == MODE_SHORT_INSTRUCTION) {
		strcpy(Current_MSG[MSG_TYPE], " GROUP ");// PH: Add "GROUP" in stead of "INSTR"
	} else {
		strcpy(Current_MSG[MSG_TYPE], vtype[vt]);	// Add flex format.
	}
}

void display_show_char(int cin) {
	if (cin == '\n') {
	} else if (cin > 127) {
		cin = '?';// PH: Display a questionmark instead of 'unknown' characters
		containsUnknownCharacters = 1;
	} else if ((cin > 0 && cin < 32 && cin != 10) && (cin != 23 && cin != 4)) {
		cin = '?';// PH: Display a questionmark instead of 'unknown' characters
		containsUnknownCharacters = 1;
	}
	message_buffer[iMessageIndex] = cin;

	if (iMessageIndex < MAX_STR_LEN - 1)
		iMessageIndex++;
} // end of display_show_char


void show_address(long int l, long int l2, int bLongAddress) {
//	long int capcode;

	if (!bLongAddress) {
		//printf("capcode long\n");
		capcode = (l & 0x1fffffl) - 32768l;
	} else {
		capcode = (l2 & 0x1fffffl) ^ 0x1fffffl;
		capcode = capcode << 15;
		capcode = capcode + 2068480l + (l & 0x1fffffl);

	}

	if ((l > 0x3fffffl) || (l2 > 0x3fffffl) || (capcode < 0)) {
		strcpy(Current_MSG[MSG_CAPCODE],
				bLongAddress ? "?????????" : "???????");
		capcode = 9999999;
	} else {
		/*if(bLongAddress)
		 {
		 printf("capcode: %09li\n", capcode);
		 } else
		 {
		 printf("capcode: %07li\n", capcode);
		 }
		 //printf("capcode: %07li\n", capcode);
		 */
		sprintf(Current_MSG[MSG_CAPCODE], bLongAddress ? "%09li" : "%07li",
				capcode);
		//printf("capcode: %s\n", Current_MSG[MSG_CAPCODE]);
	}

	if (capcode >= 2029568 && capcode <= 2029583) {
		//printf("This is a groupmessage.\n");
		bFLEX_isGroupMessage = 1;
	} else {
		iMessagesCounter++;
		bFLEX_isGroupMessage = 0;
	}

	time_t currentTime = time(NULL);
	struct tm * currentTimeInfo;

	currentTimeInfo = localtime(&currentTime);

	strftime(Current_MSG[MSG_DATE], 50, "%d-%m-%Y", currentTimeInfo);
	strftime(Current_MSG[MSG_TIME], 50, "%H:%M:%S", currentTimeInfo);
	sprintf(Current_MSG[MSG_TIMESTAMP], "%d", (int) currentTime);
}

void showframe(int asa, int vsa, struct l2_state_flex *rx) {
	int bLongAddress = 0;
	FlexTempAddress = -1;

	int iFragmentNumber, iAssignedFrame;

	if (xsumchk(frame[0]) == 0) {
		for (int j = asa; j < vsa; j++) {
			long int cc2 = frame[j] & 0x1fffffl;

			// check for long addresses (bLongAddress indicates long address)
			if (cc2 < 0x008001l)
				bLongAddress = 1;
			else if ((cc2 > 0x1e0000l) && (cc2 < 0x1f0001l))
				bLongAddress = 1;
			else if (cc2 > 0x1f7FFEl)
				bLongAddress = 1;

			int vb = vsa + j - asa;
			int vt = (frame[vb] >> 4) & 0x07;

			if (xsumchk(frame[vb]) != 0) {
				rx->flex_total_error_count++;
				continue;
			}
			//printf("\nVT: %d\n\n", vt);

			switch (vt) {
			default:

				continue;

				show_address(frame[j], frame[j + 1], bLongAddress);
				strcpy(Current_MSG[MSG_TYPE], vtype[vt]);
				//fprintf(dataFile, "Detected %s\n", Current_MSG[MSG_TYPE]);
				break;
			case MODE_ALPHA:
			case MODE_SECURE:

				show_address(frame[j], frame[j + 1], bLongAddress);
				show_phase_speed(vt);
				int w1, w2, k, c = 0;
				//int iFragmentNumber, iAssignedFrame;
				long int cc;
				//long int cc2, cc3;
				// get start and stop word numbers
				w1 = frame[vb] >> 7;
				w2 = w1 >> 7;
				w1 = w1 & 0x7f;
				w2 = (w2 & 0x7f) + w1 - 1;

				// get message fragment number (bits 11 and 12) from first header word
				// if != 3 then this is a continued message
				if (!bLongAddress) {
					iFragmentNumber = (int) (frame[w1] >> 11) & 0x03;
					w1++;
				} else {
					iFragmentNumber = (int) (frame[vb + 1] >> 11) & 0x03;
					w2--;
				}

				for (k = w1; k <= w2; k++)// dump all message characters onto screen
						{
					/*if (frame[k] > 0x3fffffl) display_color(&Pane1, COLOR_BITERRORS);
					 else display_color(&Pane1, COLOR_MESSAGE);*/

					// skip over header info (depends on fragment number)
					if ((k > w1) || (iFragmentNumber != 0x03)) {
						c = (int) frame[k] & 0x7fl;
						if (c != 0x03) {
							//printf("%c", c);
							display_show_char(c);
						}
					}

					cc = (long) frame[k] >> 7;
					c = (int) cc & 0x7fl;

					if (c != 0x03) {
						//printf("%c", c);
						display_show_char(c);
					}

					cc = (long) frame[k] >> 14;
					c = (int) cc & 0x7fl;

					if (c != 0x03) {
						//printf("%c", c);
						display_show_char(c);
					}
				}

				if (iFragmentNumber < 3)// Change last 0 of bitrate into fragmentnumber
						{
					//Current_MSG[MSG_BITRATE][3] = '1' + iFragmentNumber;
				}

				break;

			case MODE_SHORT_INSTRUCTION:
				show_address(frame[j], frame[j + 1], bLongAddress);	// show address
				if (bFLEX_isGroupMessage) {
					//printf("");
					//continue;
				}
				show_phase_speed(vt);
				iAssignedFrame = (frame[vb] >> 10) & 0x7f;// Frame with groupmessage
				FlexTempAddress = (frame[vb] >> 17) & 0x7f;	// Listen to this groupcode
				break;

			}

			if (vt == MODE_SHORT_INSTRUCTION || bFLEX_isGroupMessage) {
				if (bFLEX_isGroupMessage) {
					ConvertGroupcall(capcode - 2029568, vtype[vt], capcode);
				} else {
					AddAssignment(iAssignedFrame, FlexTempAddress, capcode);
				}
			} else {
				parseSingleMessage();
				ShowMessage();
			}

			if (bLongAddress)
				j++;

		}
	}

}

// format a received frame
void showblock(int blknum, struct l2_state_flex *rx) {
	int j, k, err, asa, vsa;
	long int cc;
	static int last_frame;
	int bNoMoreData = 0;	// Speed up frame processing

	for (int i = 0; i < 8; i++)	// format 32 bit frame into output buffer to do error correction
			{
		for (j = 0; j < 32; j++) {
			k = (j * 8) + i;
			ob[j] = block[k];
		}

		err = ecd();		// do error correction
		rx->flex_total_error_count += err;

		k = (blknum << 3) + i;

		cc = 0x0000l;

		for (j = 0; j < 21; j++) {
			cc = cc >> 1;
			if (ob[j] == 0)
				cc ^= 0x100000l;
		}

		if (err == 3)
			cc ^= 0x400000l; // flag uncorrectable errors

		frame[k] = cc;
	}
	if ((flex_speed == STAT_FLEX1600)
			&& ((cc == 0x0000l) || (cc == 0x1fffffl))) {
		bNoMoreData = 1;// Speed up frame processing
	}

	vsa = (int) ((frame[0] >> 10) & 0x3f);// get word where vector  field starts (6 bits)
	asa = (int) ((frame[0] >> 8) & 0x03) + 1;// get word where address field starts (2 bits)

	if (blknum == 0) {
		FlexTIME(rx);

		if ((iCurrentFrame == 0) && (last_frame == 127)) {
			FlexTIME(rx);
		}
		last_frame = iCurrentFrame;
	}
	else if (((blknum == 10) || bNoMoreData)) {
		showframe(asa, vsa, rx);
		if (bNoMoreData)
			iFlexBlock = 1;
	}
}

int bit10(int gin) {
	int k = 0;

	for (int i = 0; i < 10; i++) {
		if ((gin & 0x01) != 0)
			k++;
		gin = gin >> 1;
	}
	return (k);
}

int nOnes(int k) {
	int kt = 0;

	if (k == 0)
		return (0);

	for (int i = 0; i <= 15; i++) {
		if ((k & 0x0001) != 0)
			kt++;
		k = k >> 1;
	}
	return (kt);
}

//int nOnes(char k)
//{
//	int kt=0;
//
//	if (k == 0) return(0);
//
//	for (int i=0; i<=7; i++)
//	{
//		if ((k & 0x01) != 0) kt++;
//		k = k >> 1;
//	}
//	return(kt);
//}

void flex_rxbit(struct demod_state *s, int input) {

	static short int iBitBuffer[4] = { 0, 0, 0, 0 };
	static int cy, fr;
	static int bct, hbit;

	int nh;
	double aver = 0.0;
	int ihd;
	int hd = 0;

	for (int i = 0; i < 3; i++) {
		iBitBuffer[i] = iBitBuffer[i] << 1;
		if (iBitBuffer[i + 1] & 0x8000) {
			iBitBuffer[i] |= 0x0001;
		}
	}

	iBitBuffer[3] = iBitBuffer[3] << 1;

	if (input < 2) {
		iBitBuffer[3] |= 0x0001;
	}

	if (iFlexBlock == 0) {

		if (iFlexTimer) {
			if (nOnes(iBitBuffer[2] ^ EOT1) + nOnes(iBitBuffer[3] ^ EOT2) == 0) {
				iFlexTimer = 0;
				return;
			}
		}

		nh = nOnes(iBitBuffer[1] ^ SYNC1) + nOnes(iBitBuffer[2] ^ SYNC2);

		if (nh == 32) {
			if (((iBitBuffer[0] ^ iBitBuffer[3]) & 0xFFFF) == 0xFFFF) {
				iBitBuffer[0] ^= 0xFFFF;
				iBitBuffer[3] ^= 0xFFFF;
				nh = 0;
			}
		}

		if (nh < 2) {
			iFlexBlockCount = 89;

			int nh2 = nOnes(iBitBuffer[0] ^ iBitBuffer[3] ^ 0xFFFF);

			if (nh2 < 2) {
				for (int speed = 0; speed < 8; speed++) {
					if ((nOnes(iBitBuffer[0] ^ syncs[speed])
							+ nOnes(iBitBuffer[3] ^ ~syncs[speed])) < 2) {
						if ((speed & 0x03) == 0) {
						}

						iFlexTimer = 20;

						g_sps = (speed & 0x01) ? 3200 : 1600;
						level = (speed & 0x02) ? 4 : 2;
						break;

					}
				}
			} else {
				return;
			}

			for (int j = 0; j < 64; j++) {
				aver = aver + rcver[j];
			}
			aver *= 0.015625;
			aver *= 0.5;
			exc += aver;
		}

		if (iFlexBlockCount > 0) {
			iFlexBlockCount--;
			if ((iFlexBlockCount < 72) && (iFlexBlockCount > 39)) {
				if (input < 2) {
					ob[71 - iFlexBlockCount] = 1;
				} else {
					ob[71 - iFlexBlockCount] = 0;
				}
			} else if (iFlexBlockCount == 39) {
				int cer = ecd();

				if (cer < 2) {
					for (ihd = 4; ihd < 8; ihd++) {
						hd = hd >> 1;
						if (ob[ihd] == 1) {
							hd ^= 0x08;
						}
					}
					cy = (hd & 0x0f) ^ 0x0f;
					iCurrentCycle = cy;

					for (ihd = 8; ihd <= 14; ihd++) {
						hd = hd >> 1;
						if (ob[ihd] == 1) {
							hd ^= 0x40;
						}
					}
					fr = (hd & 0x7f) ^ 0x7f;
					iCurrentFrame = fr;
				}

			}

			if (iFlexBlockCount == 0) {
				iFlexBlock = 11;
				bct = 0;
				hbit = 0;
			}

		}

	} else {
		if (g_sps == 1600) {
			if (input < 2)
				block[bct] = 1;
			else
				block[bct] = 0;
			bct++;
		} else {
			if (hbit == 0) {
				if (input < 2)
					block[bct] = 1;
				else
					block[bct] = 0;
				hbit++;
			} else {
				hbit = 0;
				bct++;
			}
		}

		if (bct == 256) {
			bct = 0;

			showblock(11 - iFlexBlock, &s->l2.flex);

			iFlexBlock--;
			if (iFlexBlock == 0) {
				iFrameCount++;
			}
		}

	}

}

void flex_init(struct demod_state *s) {
	memset(&s->l2.flex, 0, sizeof(s->l2.flex));
	setupecc();
	for (int i = 0; i < 64; i++) {
		rcver[i] = 0.0;
	}
}

void flex_deinit(struct demod_state *s)
{
    if(s->l2.flex.flex_total_error_count)
        verbprintf(1, "\n===FLEX stats===\n"
                   "Total error count: %u\n",
                   s->l2.flex.flex_total_error_count);
    fflush(stdout);
}
