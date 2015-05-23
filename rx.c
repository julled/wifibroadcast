// (c)2015 befinitiv

/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; version 2.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License along
 *   with this program; if not, write to the Free Software Foundation, Inc.,
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */



#include "lib.h"
#include "wifibroadcast.h"
#include "radiotap.h"

#define MAX_PACKET_LENGTH 4192

// this is where we store a summary of the
// information from the radiotap header

typedef struct  {
	int m_nChannel;
	int m_nChannelFlags;
	int m_nRate;
	int m_nAntenna;
	int m_nRadiotapFlags;
} __attribute__((packed)) PENUMBRA_RADIOTAP_DATA;



int flagHelp = 0;
int param_port = 0;
int param_retransmission_block_size = 1;
int last_block_num = -1;
int num_sent = 0, num_lost = 0;

void
usage(void)
{
	printf(
	    "(c)2015 befinitiv. Based on packetspammer by Andy Green.  Licensed under GPL2\n"
	    "\n"
	    "Usage: rx [options] <interfaces>\n\nOptions\n"
			"-p <port> Port number 0-255 (default 0)\n"
			"-b <blocksize> Number of packets in a retransmission block (default 1). Needs to match with tx.\n"
	    "Example:\n"
	    "  echo -n mon0 > /sys/class/ieee80211/phy0/add_iface\n"
	    "  iwconfig mon0 mode monitor\n"
	    "  ifconfig mon0 up\n"
	    "  rx mon0        Receive raw packets on mon0 and output the payload to stdout\n"
	    "\n");
	exit(1);
}

typedef struct {
	pcap_t *ppcap;
	int selectable_fd;
	int n80211HeaderLength;
} monitor_interface_t;

void open_and_configure_interface(const char *name, int port, monitor_interface_t *interface) {
	struct bpf_program bpfprogram;
	char szProgram[512];
	char szErrbuf[PCAP_ERRBUF_SIZE];
		// open the interface in pcap

	szErrbuf[0] = '\0';
	interface->ppcap = pcap_open_live(name, 2048, 1, 0, szErrbuf);
	if (interface->ppcap == NULL) {
		fprintf(stderr, "Unable to open interface %s in pcap: %s\n",
		    name, szErrbuf);
		exit(1);
	}
	

	if(pcap_setnonblock(interface->ppcap, 0, szErrbuf) < 0) {
		fprintf(stderr, "Error setting %s to nonblocking mode: %s\n", name, szErrbuf);
	}

	int nLinkEncap = pcap_datalink(interface->ppcap);

	switch (nLinkEncap) {

		case DLT_PRISM_HEADER:
			fprintf(stderr, "DLT_PRISM_HEADER Encap\n");
			interface->n80211HeaderLength = 0x20; // ieee80211 comes after this
			sprintf(szProgram, "radio[0x4a:4]==0x13223344 && radio[0x4e:2] == 0x55%.2x", port);
			break;

		case DLT_IEEE802_11_RADIO:
			fprintf(stderr, "DLT_IEEE802_11_RADIO Encap\n");
			interface->n80211HeaderLength = 0x18; // ieee80211 comes after this
			sprintf(szProgram, "ether[0x0a:4]==0x13223344 && ether[0x0e:2] == 0x55%.2x", port);
			break;

		default:
			fprintf(stderr, "!!! unknown encapsulation on %s !\n", name);
			exit(1);

	}

	if (pcap_compile(interface->ppcap, &bpfprogram, szProgram, 1, 0) == -1) {
		puts(szProgram);
		puts(pcap_geterr(interface->ppcap));
		exit(1);
	} else {
		if (pcap_setfilter(interface->ppcap, &bpfprogram) == -1) {
			fprintf(stderr, "%s\n", szProgram);
			fprintf(stderr, "%s\n", pcap_geterr(interface->ppcap));
		} else {
		}
		pcap_freecode(&bpfprogram);
	}

	interface->selectable_fd = pcap_get_selectable_fd(interface->ppcap);
}


void process_packet(monitor_interface_t *interface, packet_buffer_t *packet_buffer_list) {
		struct pcap_pkthdr * ppcapPacketHeader = NULL;
		struct ieee80211_radiotap_iterator rti;
		PENUMBRA_RADIOTAP_DATA prd;
		u8 payloadBuffer[MAX_PACKET_LENGTH];
		u8 *pu8Payload = payloadBuffer;
		int bytes;
		int n;
		uint32_t seq_nr;
		int block_num;
		int packet_num;
		int checksum_correct;
		int retval;
		int u16HeaderLen;
		int i;

		// receive


		retval = pcap_next_ex(interface->ppcap, &ppcapPacketHeader,
		    (const u_char**)&pu8Payload);

		if (retval < 0) {
			fprintf(stderr, "Socket broken\n");
			fprintf(stderr, "%s\n", pcap_geterr(interface->ppcap));
			exit(1);
		}
		if (retval != 1)
			return;


		u16HeaderLen = (pu8Payload[2] + (pu8Payload[3] << 8));

		if (ppcapPacketHeader->len <
		    (u16HeaderLen + interface->n80211HeaderLength))
			return;

		bytes = ppcapPacketHeader->len -
			(u16HeaderLen + interface->n80211HeaderLength);
		if (bytes < 0)
			return;

		if (ieee80211_radiotap_iterator_init(&rti,
		    (struct ieee80211_radiotap_header *)pu8Payload,
		    ppcapPacketHeader->len) < 0)
			return;

		while ((n = ieee80211_radiotap_iterator_next(&rti)) == 0) {

			switch (rti.this_arg_index) {
			case IEEE80211_RADIOTAP_RATE:
				prd.m_nRate = (*rti.this_arg);
				break;

			case IEEE80211_RADIOTAP_CHANNEL:
				prd.m_nChannel =
				    le16_to_cpu(*((u16 *)rti.this_arg));
				prd.m_nChannelFlags =
				    le16_to_cpu(*((u16 *)(rti.this_arg + 2)));
				break;

			case IEEE80211_RADIOTAP_ANTENNA:
				prd.m_nAntenna = (*rti.this_arg) + 1;
				break;

			case IEEE80211_RADIOTAP_FLAGS:
				prd.m_nRadiotapFlags = *rti.this_arg;
				break;
			}
		}
		pu8Payload += u16HeaderLen + interface->n80211HeaderLength;

		if (prd.m_nRadiotapFlags & IEEE80211_RADIOTAP_F_FCS)
			bytes -= 4;


		checksum_correct = (prd.m_nRadiotapFlags & 0x40) == 0; 

		//first 4 bytes are the sequence number
		seq_nr = *(uint32_t*)pu8Payload;
		pu8Payload += 4;
		bytes -= 4;

		block_num = seq_nr / param_retransmission_block_size;//if retr_block_size would be limited to powers of two, this could be replaced by a logical AND operation

		//fprintf(stderr, "rec %x blk %x\n", seq_nr, block_num);
		//if we received the start of a new block, we need to write out the old one
		if(block_num > last_block_num && last_block_num >= 0 && checksum_correct) { 
			
			//write out old block
			for(i=0; i<param_retransmission_block_size; ++i) {
				packet_buffer_t *p = packet_buffer_list + i;
				num_sent++;
				if(p->valid) {
					write(STDOUT_FILENO, p->data, p->len);
				}
				else {
					fprintf(stderr, "Lost a packet %x! Lossrate: %f\t(%d / %d)\n", i+(block_num-1)*param_retransmission_block_size, 1.0 * num_lost/num_sent, num_lost, num_sent);
					num_lost++;
				}

				p->valid = 0;
				p->crc_correct = 0;
				p->len = 0;
			}
			if(block_num > last_block_num + 1) {
				int lost_blocks = block_num - last_block_num - 1;
				num_lost += lost_blocks * param_retransmission_block_size;
				num_sent += lost_blocks * param_retransmission_block_size;
				fprintf(stderr, "Lost %d blocks! Lossrate %f\t(%d / %d)\n", block_num - last_block_num - 1, 1.0 * num_lost/num_sent, num_lost, num_sent);
			}
		}
	
		//safety first: we only go to the next block if the FCS is correct
		if(checksum_correct && block_num > last_block_num)
			last_block_num = block_num;
		
		packet_num = seq_nr % param_retransmission_block_size; //if retr_block_size would be limited to powers of two, this could be replace by a locical and operation

		//only overwrite packets where the checksum is not yet correct. otherwise the packets are already received correctly
		if(packet_buffer_list[packet_num].crc_correct == 0 && block_num == last_block_num) {
			memcpy(packet_buffer_list[packet_num].data, pu8Payload, bytes);
			packet_buffer_list[packet_num].len = bytes;
			packet_buffer_list[packet_num].valid = 1;
			packet_buffer_list[packet_num].crc_correct = checksum_correct;
		}
}

int
main(int argc, char *argv[])
{
	monitor_interface_t interfaces[MAX_PENUMBRA_INTERFACES];
	int num_interfaces = 0;

	packet_buffer_t *packet_buffer_list;


	while (1) {
		int nOptionIndex;
		static const struct option optiona[] = {
			{ "help", no_argument, &flagHelp, 1 },
			{ 0, 0, 0, 0 }
		};
		int c = getopt_long(argc, argv, "hp:b:",
			optiona, &nOptionIndex);

		if (c == -1)
			break;
		switch (c) {
		case 0: // long option
			break;

		case 'h': // help
			usage();

		case 'p': //port
			param_port = atoi(optarg);
			break;
		
		case 'b': //retransmission block size
			param_retransmission_block_size = atoi(optarg);
			break;

		default:
			fprintf(stderr, "unknown switch %c\n", c);
			usage();
			break;
		}
	}

	if (optind >= argc)
		usage();

	int x = optind;
	while(x < argc && num_interfaces < MAX_PENUMBRA_INTERFACES) {
		open_and_configure_interface(argv[x], param_port, interfaces + num_interfaces);
		++num_interfaces;
		++x;
	}

	packet_buffer_list = lib_alloc_packet_buffer_list(param_retransmission_block_size, MAX_PACKET_LENGTH);


	for(;;) { 
		int i;
/*		fd_set readset;
	
		FD_ZERO(&readset);
		for(i=0; i<num_interfaces; ++i)
			FD_SET(interfaces[i].selectable_fd, &readset);

		int n = select(30, &readset, NULL, NULL, NULL);
		printf("select n = %d\n", n);
*/
		for(i=0; i<num_interfaces; ++i) {
//			if(n == 0)
//				break;

			//printf("is %d set\n", interfaces[i].selectable_fd);
			{//if(FD_ISSET(interfaces[i].selectable_fd, &readset)) {
				process_packet(interfaces + i, packet_buffer_list);
			}
		}

	}

	return (0);
}
