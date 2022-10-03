// Copyright 2011 Juri Glass, Mathias Runge, Nadim El Sayed
// DAI-Labor, TU-Berlin
//
// This file is part of libSML.
// Thanks to Thomas Binder and Axel (tuxedo) for providing code how to
// print OBIS data (see transport_receiver()).
// https://community.openhab.org/t/using-a-power-meter-sml-with-openhab/21923
//
// libSML is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// libSML is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with libSML.  If not, see <http://www.gnu.org/licenses/>.

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <ctype.h>
#include <errno.h>
#include <termios.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <sys/ioctl.h>
#include <mosquitto.h>

#include <sml/sml_file.h>
#include <sml/sml_transport.h>
#include <sml/sml_value.h>

#include "unit.h"

// globals
int sflag = false; // flag to process only a single OBIS data stream
int vflag = false; // verbose flag
int mflag = false; // MQTT flag
int rc;
struct mosquitto *mosq;


int serial_port_open(const char* device) {
	int bits;
	struct termios config;
	memset(&config, 0, sizeof(config));

	if (!strcmp(device, "-"))
		return 0; // read stdin when "-" is given for the device

#ifdef O_NONBLOCK
	int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
#else
	int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
#endif
	if (fd < 0) {
		fprintf(stderr, "error: open(%s): %s\n", device, strerror(errno));
		return -1;
	}

	// set RTS
	ioctl(fd, TIOCMGET, &bits);
	bits |= TIOCM_RTS;
	ioctl(fd, TIOCMSET, &bits);

	tcgetattr(fd, &config);

	// set 8-N-1
	config.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR
			| ICRNL | IXON);
	config.c_oflag &= ~OPOST;
	config.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
	config.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
	config.c_cflag |= CS8;

	// set speed to 9600 baud
	cfsetispeed(&config, B9600);
	cfsetospeed(&config, B9600);

	tcsetattr(fd, TCSANOW, &config);
	return fd;
}

void transport_receiver(unsigned char *buffer, size_t buffer_len) {
	int i, max_len=100, res;
	const char topic_const[] = "smartmeter/mains/sensor/1/obis/%d-%d:%d.%d.%d/%d/value";
	char topic[max_len];
	// the buffer contains the whole message, with transport escape sequences.
	// these escape sequences are stripped here.
	sml_file *file = sml_file_parse(buffer + 8, buffer_len - 16);
	// the sml file is parsed now

	// this prints some information about the file
	if (vflag)
		sml_file_print(file);

	// read here some values ...
	if (vflag)
		printf("OBIS data\n");
	for (i = 0; i < file->messages_len; i++) {
		sml_message *message = file->messages[i];
		if (*message->message_body->tag == SML_MESSAGE_GET_LIST_RESPONSE) {
			sml_list *entry;
			sml_get_list_response *body;
			body = (sml_get_list_response *) message->message_body->data;
			for (entry = body->val_list; entry != NULL; entry = entry->next) {
				if (!entry->value) { // do not crash on null value
					fprintf(stderr, "Error in data stream. entry->value should not be NULL. Skipping this.\n");
					continue;
				}
				res = MOSQ_ERR_SUCCESS;
				if (entry->value->type == SML_TYPE_OCTET_STRING) {
					char *str;
					sml_value_to_strhex(entry->value, &str, true);
					if(mflag) {
						snprintf(topic, max_len, topic_const,
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5]);
						res = mosquitto_publish(mosq, NULL, topic, strlen(str), str, 0, false);
					}
					if(!mflag || vflag) {
						printf("%d-%d:%d.%d.%d*%d#%s#\n",
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5],
								str);
					}
					free(str);
				} else if (entry->value->type == SML_TYPE_BOOLEAN) {
					const char *str;
					if(entry->value->data.boolean) {
						str = "true";
					} else {
						str = "false";
					}
					if(mflag) {
						snprintf(topic, max_len, topic_const,
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5]);
						res = mosquitto_publish(mosq, NULL, topic, strlen(str), str, 0, false);
					}
					if(!mflag || vflag) {
						printf("%d-%d:%d.%d.%d*%d#%s#\n",
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5],
								str);
					}
				} else if (((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_INTEGER) ||
						((entry->value->type & SML_TYPE_FIELD) == SML_TYPE_UNSIGNED)) {
					double value = sml_value_to_double(entry->value);
					int scaler = (entry->scaler) ? *entry->scaler : 0;
					int prec = -scaler;
					char str[max_len];
					if (prec < 0)
						prec = 0;
					value = value * pow(10, scaler);
					if(mflag) {
						snprintf(topic, max_len, topic_const,
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5]);
						snprintf(str, max_len, "%.*f", prec, value);
						res = mosquitto_publish(mosq, NULL, topic, strlen(str), str, 0, false);
					}
					if(!mflag || vflag) {
						printf("%d-%d:%d.%d.%d*%d#%.*f#",
								entry->obj_name->str[0], entry->obj_name->str[1],
								entry->obj_name->str[2], entry->obj_name->str[3],
								entry->obj_name->str[4], entry->obj_name->str[5], prec, value);
						const char *unit = NULL;
						if (entry->unit &&  // do not crash on null (unit is optional)
							(unit = dlms_get_unit((unsigned char) *entry->unit)) != NULL)
							printf("%s", unit);
						printf("\n");
						// flush the stdout puffer, that pipes work without waiting
						fflush(stdout);
					}
				}
				if(res != MOSQ_ERR_SUCCESS) {
					printf("Reconnect to broker\n");
					mosquitto_reconnect(mosq);
				}
			}
			if (sflag)
				exit(0); // processed first message - exit
		}
	}

	// free the malloc'd memory
	sml_file_free(file);
}

int main(int argc, char *argv[]) {
	// this example assumes that a EDL21 meter sending SML messages via a
	// serial device. Adjust as needed.
	int c;

	while ((c = getopt(argc, argv, "+hsvm")) != -1) {
		switch (c) {
		case 'h':
			printf("usage: %s [-h] [-s] [-v] [-m] device\n", argv[0]);
			printf("device - serial device of connected power meter e.g. /dev/cu.usbserial, or - for stdin\n");
			printf("-h - help\n");
			printf("-s - process only one OBIS data stream (single)\n");
			printf("-v - verbose\n");
			printf("-m - use MQTT\n");
			exit(0); // exit here
			break;
		case 's':
			sflag = true;
			break;
		case 'v':
			vflag = true;
			break;
		case 'm':
			mflag = true;
			break;
		case '?':
			// get a not specified switch, error message is printed by getopt()
			printf("Use %s -h for help.\n", argv[0]);
			exit(1); // exit here
			break;
		default:
			break;
		}
	}

	if (argc - optind != 1) {
		printf("error: Arguments mismatch.\nUse %s -h for help.\n", argv[0]);
		exit(1); // exit here
	}

	// open serial port
	int fd = serial_port_open(argv[optind]);
	if (fd < 0) {
		// error message is printed by serial_port_open()
		exit(1);
	}

	if (mflag) {
		mosquitto_lib_init();
		mosq = mosquitto_new("smartmeter", true, NULL);
		rc = mosquitto_connect(mosq, "localhost", 1883, 60);
		if(rc != 0) {
			printf("error: Mosquitto client could not connect to broker! Error Code: %d\n", rc);
			mosquitto_destroy(mosq);
			close(fd);
			exit(1);
		}
		printf("We are now connected to the broker!\n");
	}

	// listen on the serial device, this call is blocking.
	sml_transport_listen(fd, &transport_receiver);
	close(fd);

	if(mflag) {
		mosquitto_disconnect(mosq);
		mosquitto_destroy(mosq);
		mosquitto_lib_cleanup();
	}
	
	return 0;
}
