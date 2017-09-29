/*----------------------------------------------------------------------------*/
#include "my1stc.h"
#include "my1cons.h"
//#include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
#include <sys/time.h>
/*----------------------------------------------------------------------------*/
int stc_wait_packet(stc_dev_t* pdevice, serial_port_t* pport)
{
	int temp;
	pdevice->pcount = 0;
	while(1)
	{
		temp = get_byte_serial_timed(pport,pdevice->timeout_us);
		if(temp==SERIAL_TIMEOUT)
			break;
		if(pdevice->pcount<STC_PACKET_SIZE)
			pdevice->packet[pdevice->pcount++] = (byte_t) temp;
		else
			break;
		if (temp==STC_PACKET_ME) break; /* stop if we find end marker! */
	}
	return temp==SERIAL_TIMEOUT?temp:pdevice->pcount;
}
/*----------------------------------------------------------------------------*/
int stc_check_isp(stc_dev_t* pdevice, serial_port_t* pport)
{
	struct timeval inittime, currtime;
	my1key_t key;
	int do_wait = 0;
	int status = STC_SYNC_INIT;
	while(status<STC_SYNC_DONE)
	{
		if(!do_wait)
		{
			put_byte_serial(pport,STC_SYNC_CHAR);
			gettimeofday(&inittime,0x0);
			do_wait = 1;
		}
		else
		{
			/* gettimeofday is using micro-second resolution! */
			gettimeofday(&currtime,0x0);
			if((currtime.tv_sec>inittime.tv_sec)||
				(currtime.tv_usec-inittime.tv_usec)>pdevice->timeout_us)
			{
				do_wait = 0;
			}
		}
		if(check_incoming(pport))
		{
			if (stc_wait_packet(pdevice,pport)==SERIAL_TIMEOUT)
				status = STC_SYNC_MISS;
			else
				status = STC_SYNC_DONE;
			break;
		}
		key = get_keyhit();
		if(key==KEY_ESCAPE)
			status = STC_SYNC_NONE;
		else if(key==KEY_BSPACE)
			status = STC_SYNC_REST;
	}
	return status;
}
/*----------------------------------------------------------------------------*/
unsigned short stc_generate_chksum(stc_dev_t* pdevice)
{
	int loop;
	unsigned short csum = 0x0000;
	for(loop=2;loop<pdevice->pcount-3;loop++)
		csum += pdevice->packet[loop];
	return csum;
}
/*----------------------------------------------------------------------------*/
unsigned short change_endian(unsigned short test)
{
	unsigned char *byte = (unsigned char*) &test;
	unsigned short swap = byte[0];
	return (swap<<8)|byte[1];
}
/*----------------------------------------------------------------------------*/
int stc_validate_packet(stc_dev_t* pdevice)
{
	int test = STC_PACKET_VALID, loop;
	/* 16-bit big-endian init marker */
	pdevice->info.imark = ((int)pdevice->packet[0]<<8) + pdevice->packet[1];
	/* direction */
	pdevice->info.hflag = (int)pdevice->packet[2];
	/* 16-bit big-endian length */
	pdevice->info.psize = ((int)pdevice->packet[3]<<8) + pdevice->packet[4];
	/* assign data pointer */
	pdevice->info.pdata = &pdevice->packet[5];
	pdevice->info.dsize = pdevice->info.psize - 6;
	/* check packet size for 'implied' end marker? */
	loop = pdevice->pcount;
	if (loop==STC_PACKET_SIZE)
	{
		/* end marker is implied */
		pdevice->info.emark = STC_PACKET_ME;
	}
	else
	{
		loop--;
		pdevice->info.emark = pdevice->packet[loop];
	}
	/* 16-bit big-endian checksum */
	pdevice->info.cksum = ((int)pdevice->packet[loop-2]<<8) +
		pdevice->packet[loop-1];
	/* do validation! */
	if (pdevice->info.imark!=STC_PACKET_MX)
		test = STC_PACKET_ERROR_BEGMARK;
	else if (pdevice->info.hflag!=STC_PACKET_MCU2HOST)
		test = STC_PACKET_ERROR_DIRECT;
	else if (pdevice->info.psize!=pdevice->pcount-2)
		test = STC_PACKET_ERROR_LENGTH;
	else if (pdevice->info.emark!=STC_PACKET_ME)
		test = STC_PACKET_ERROR_ENDMARK;
	else if (pdevice->info.cksum!=stc_generate_chksum(pdevice))
		test = STC_PACKET_ERROR_CHECKSUM;
	return test;
}
/*----------------------------------------------------------------------------*/
int stc_check_info(stc_dev_t* pdevice)
{
	int loop, test = stc_validate_packet(pdevice);
	if (test==STC_PACKET_VALID)
	{
		stc_payload_info_t *info = (stc_payload_info_t*) pdevice->info.pdata;
		//pdevice->flag = pdevice->info.pdata[PAYLOAD_INFO_OFFSET_FLAG];
		pdevice->flag = info->flag;
		pdevice->freq = 0.0;
		//unsigned char *byte = &pdevice->info.pdata[PAYLOAD_INFO_OFFSET_SYNC];
		for (loop=0;loop<8;loop++)
		{
			//temp = (int)(byte[0])<<8;
			//temp |= (int)(byte[1]);
			//pdevice->freq += (float)temp;
			pdevice->freq += (float)change_endian(info->sync[loop]);
			//byte++; byte++;
		}
		pdevice->freq /= 8; /* get average */
		/* formula from stcdude */
		pdevice->freq = (pdevice->freq * 9600 * 12)/(7*1000000);
		//pdevice->uid0 = pdevice->info.pdata[PAYLOAD_INFO_OFFSET_MID1];
		//pdevice->uid1 = pdevice->info.pdata[PAYLOAD_INFO_OFFSET_MID2];
		pdevice->uid0 = info->mid[0];
		pdevice->uid1 = info->mid[1];
		//pdevice->fw11 = pdevice->info.pdata[PAYLOAD_INFO_OFFSET_VER1];
		pdevice->fw11 = info->ver1;
		pdevice->fw12 = (pdevice->fw11&0x0f);
		pdevice->fw11 = (pdevice->fw11&0xf0)>>4;
		//pdevice->fw20 = pdevice->info.pdata[PAYLOAD_INFO_OFFSET_VER2];
		pdevice->fw20 = info->ver2;
	}
	return test;
}
/*----------------------------------------------------------------------------*/
int stc_handshake(stc_dev_t* pdevice, serial_port_t* pport)
{
	my1key_t key;
	int status = 0, loop;
	unsigned short temp;
	pdevice->pcount = 15;
	pdevice->packet[0] = STC_PACKET_M0;
	pdevice->packet[1] = STC_PACKET_M1;
	pdevice->packet[2] = STC_PACKET_HOST2MCU;
	temp = pdevice->pcount-2;
	pdevice->packet[3] = (unsigned char)((temp&0xff00)>>8);
	pdevice->packet[4] = (unsigned char)(temp&0xff);
	pdevice->packet[5] = pdevice->flag;
	pdevice->packet[6] = 0x00;
	pdevice->packet[7] = 0x00;
	pdevice->packet[8] = 0x36;
	pdevice->packet[9] = 0x01;
	pdevice->packet[10] = (unsigned char)(pdevice->uid0&0xff);
	pdevice->packet[11] = (unsigned char)(pdevice->uid1&0xff);
	/* get checksum */
	temp = stc_generate_chksum(pdevice);
	pdevice->packet[12] = (unsigned char)((temp&0xff00)>>8);
	pdevice->packet[13] = (unsigned char)(temp&0xff);
	/* end marker */
	pdevice->packet[pdevice->pcount-1] = STC_PACKET_ME;
	/* send packet */
	for (loop=0;loop<pdevice->pcount;loop++)
	{
#include <stdio.h>
		printf("[%02x]",pdevice->packet[loop]);
		put_byte_serial(pport,pdevice->packet[loop]);
	}
	/* wait for response */
	while(1)
	{
		if(check_incoming(pport))
		{
			if (stc_wait_packet(pdevice,pport)==SERIAL_TIMEOUT)
				status = -1;
			break;
		}
		key = get_keyhit();
		if(key==KEY_ESCAPE)
		{
			status = -2;
			break;
		}
	}
	return status<0?status:pdevice->pcount;
}
/*----------------------------------------------------------------------------*/
