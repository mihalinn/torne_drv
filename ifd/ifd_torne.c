// SPDX-License-Identifier: GPL-2.0-only
/*
 * ifd_torne.c - pcscd reader driver (IFD handler) for the card slot of the Sony CECH-ZD1 J
 *
 * The slot is not a CCID reader; the torne_drv kernel module speaks T=1 to the card and offers APDU-level calls on
 * /dev/tornecardN (torne_ioctl.h). This library hands them to pcscd, so recisdb, ts-filter and other PC/SC programs
 * (the B-CAS decoders of Mirakurun and EDCB) see the slot as a normal reader.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <PCSC/ifdhandler.h>
#include <PCSC/debuglog.h>
#include <PCSC/reader.h>

#include "torne_ioctl.h"

#define MAX_READERS 16
#define SLOT_NAME_LEN 64

struct reader {
	int fd;
	char path[SLOT_NAME_LEN];
	UCHAR atr[40];
	DWORD atr_len;
};

static struct reader readers[MAX_READERS];
static pthread_mutex_t readers_lock = PTHREAD_MUTEX_INITIALIZER;

static struct reader *reader_of(DWORD lun)
{
	unsigned int index = lun >> 16;

	if (index >= MAX_READERS || readers[index].fd < 0)
		return NULL;
	return &readers[index];
}

static void init_readers(void)
{
	static int done;
	int i;

	if (done)
		return;
	for (i = 0; i < MAX_READERS; i++)
		readers[i].fd = -1;
	done = 1;
}

/* the USB bus and device number of /dev/tornecardN, from sysfs; -1 when unknown */
static int usb_numbers(int card, int *bus, int *dev)
{
	char path[128];
	FILE *f;

	*bus = *dev = -1;
	snprintf(path, sizeof(path), "/sys/class/misc/tornecard%d/device/../busnum", card);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", bus) != 1)
		*bus = -1;
	fclose(f);
	snprintf(path, sizeof(path), "/sys/class/misc/tornecard%d/device/../devnum", card);
	f = fopen(path, "r");
	if (!f)
		return -1;
	if (fscanf(f, "%d", dev) != 1)
		*dev = -1;
	fclose(f);
	return 0;
}

/* pcscd names a USB reader "usb:VVVV/PPPP:libusb-1.0:BUS:DEV:IF" or "usb:VVVV/PPPP:libudev:N:/dev/bus/usb/BBB/DDD" */
static void parse_device_name(const char *name, int *bus, int *dev)
{
	const char *p;

	*bus = *dev = -1;
	p = strstr(name, "libusb-1.0:");
	if (p) {
		sscanf(p + strlen("libusb-1.0:"), "%d:%d", bus, dev);
		return;
	}
	p = strstr(name, "/dev/bus/usb/");
	if (p)
		sscanf(p + strlen("/dev/bus/usb/"), "%d/%d", bus, dev);
}

static RESPONSECODE open_reader(DWORD lun, const char *name)
{
	unsigned int index = lun >> 16;
	int want_bus = -1, want_dev = -1, card, chosen = -1, first = -1;

	init_readers();
	if (index >= MAX_READERS)
		return IFD_COMMUNICATION_ERROR;
	if (name)
		parse_device_name(name, &want_bus, &want_dev);

	pthread_mutex_lock(&readers_lock);
	if (readers[index].fd >= 0) {
		pthread_mutex_unlock(&readers_lock);
		return IFD_SUCCESS; /* another slot of the same reader */
	}
	for (card = 0; card < MAX_READERS; card++) {
		char path[SLOT_NAME_LEN];
		int bus, dev;

		snprintf(path, sizeof(path), "/dev/tornecard%d", card);
		if (access(path, F_OK) != 0)
			continue;
		if (first < 0)
			first = card;
		if (want_bus >= 0 && usb_numbers(card, &bus, &dev) == 0 && bus == want_bus && dev == want_dev) {
			chosen = card;
			break;
		}
	}
	if (chosen < 0)
		chosen = first;
	if (chosen < 0) {
		pthread_mutex_unlock(&readers_lock);
		Log1(PCSC_LOG_ERROR, "no /dev/tornecardN (is torne_drv loaded?)");
		return IFD_COMMUNICATION_ERROR;
	}
	snprintf(readers[index].path, sizeof(readers[index].path), "/dev/tornecard%d", chosen);
	readers[index].fd = open(readers[index].path, O_RDWR | O_CLOEXEC);
	readers[index].atr_len = 0;
	pthread_mutex_unlock(&readers_lock);
	if (readers[index].fd < 0) {
		Log3(PCSC_LOG_ERROR, "open %s: %s", readers[index].path, strerror(errno));
		return IFD_COMMUNICATION_ERROR;
	}
	return IFD_SUCCESS;
}

RESPONSECODE IFDHCreateChannelByName(DWORD Lun, LPSTR lpcDevice)
{
	return open_reader(Lun, lpcDevice);
}

RESPONSECODE IFDHCreateChannel(DWORD Lun, DWORD Channel)
{
	return open_reader(Lun, NULL);
}

RESPONSECODE IFDHCloseChannel(DWORD Lun)
{
	struct reader *r = reader_of(Lun);

	if (!r)
		return IFD_COMMUNICATION_ERROR;
	pthread_mutex_lock(&readers_lock);
	close(r->fd);
	r->fd = -1;
	pthread_mutex_unlock(&readers_lock);
	return IFD_SUCCESS;
}

RESPONSECODE IFDHGetCapabilities(DWORD Lun, DWORD Tag, PDWORD Length, PUCHAR Value)
{
	struct reader *r = reader_of(Lun);

	if (!r)
		return IFD_COMMUNICATION_ERROR;
	switch (Tag) {
	case TAG_IFD_ATR:
	case SCARD_ATTR_ATR_STRING:
		if (r->atr_len == 0 || *Length < r->atr_len) {
			*Length = 0;
			return IFD_ERROR_TAG;
		}
		*Length = r->atr_len;
		memcpy(Value, r->atr, r->atr_len);
		return IFD_SUCCESS;
	case TAG_IFD_SLOTS_NUMBER:
		if (*Length < 1)
			return IFD_ERROR_TAG;
		*Length = 1;
		*Value = 1;
		return IFD_SUCCESS;
	case TAG_IFD_SIMULTANEOUS_ACCESS:
		if (*Length < 1)
			return IFD_ERROR_TAG;
		*Length = 1;
		*Value = 1;
		return IFD_SUCCESS;
	case TAG_IFD_THREAD_SAFE:
	case TAG_IFD_SLOT_THREAD_SAFE:
		if (*Length < 1)
			return IFD_ERROR_TAG;
		*Length = 1;
		*Value = 0;
		return IFD_SUCCESS;
	default:
		return IFD_ERROR_TAG;
	}
}

RESPONSECODE IFDHSetCapabilities(DWORD Lun, DWORD Tag, DWORD Length, PUCHAR Value)
{
	return IFD_ERROR_TAG;
}

/* the driver speaks T=1 to the card itself; whatever protocol pcscd picks from the ATR is accepted */
RESPONSECODE IFDHSetProtocolParameters(DWORD Lun, DWORD Protocol, UCHAR Flags, UCHAR PTS1, UCHAR PTS2, UCHAR PTS3)
{
	if (!reader_of(Lun))
		return IFD_COMMUNICATION_ERROR;
	if (Protocol != SCARD_PROTOCOL_T1 && Protocol != SCARD_PROTOCOL_T0)
		return IFD_PROTOCOL_NOT_SUPPORTED;
	return IFD_SUCCESS;
}

RESPONSECODE IFDHPowerICC(DWORD Lun, DWORD Action, PUCHAR Atr, PDWORD AtrLength)
{
	struct reader *r = reader_of(Lun);
	struct torne_card_atr atr;

	if (!r)
		return IFD_COMMUNICATION_ERROR;
	switch (Action) {
	case IFD_POWER_DOWN:
		r->atr_len = 0;
		*AtrLength = 0;
		return IFD_SUCCESS;
	case IFD_POWER_UP:
	case IFD_RESET:
		memset(&atr, 0, sizeof(atr));
		if (ioctl(r->fd, TORNE_CARD_IOC_RESET, &atr) < 0) {
			*AtrLength = 0;
			r->atr_len = 0;
			Log2(PCSC_LOG_INFO, "reset: %s", strerror(errno));
			return errno == ENOMEDIUM ? IFD_ERROR_POWER_ACTION : IFD_COMMUNICATION_ERROR;
		}
		if (atr.len > sizeof(r->atr) || atr.len > *AtrLength)
			return IFD_COMMUNICATION_ERROR;
		memcpy(r->atr, atr.atr, atr.len);
		r->atr_len = atr.len;
		memcpy(Atr, atr.atr, atr.len);
		*AtrLength = atr.len;
		return IFD_SUCCESS;
	default:
		return IFD_NOT_SUPPORTED;
	}
}

RESPONSECODE IFDHTransmitToICC(DWORD Lun, SCARD_IO_HEADER SendPci, PUCHAR TxBuffer, DWORD TxLength, PUCHAR RxBuffer,
			       PDWORD RxLength, PSCARD_IO_HEADER RecvPci)
{
	struct reader *r = reader_of(Lun);
	struct torne_card_apdu apdu;

	if (!r)
		return IFD_COMMUNICATION_ERROR;
	if (TxLength == 0 || TxLength > TORNE_CARD_MAX_APDU) {
		*RxLength = 0;
		return IFD_COMMUNICATION_ERROR;
	}
	memset(&apdu, 0, sizeof(apdu));
	apdu.tx_len = TxLength;
	memcpy(apdu.tx, TxBuffer, TxLength);
	apdu.rx_len = sizeof(apdu.rx);
	if (ioctl(r->fd, TORNE_CARD_IOC_TRANSMIT, &apdu) < 0) {
		*RxLength = 0;
		Log2(PCSC_LOG_INFO, "transmit: %s", strerror(errno));
		return errno == ETIMEDOUT ? IFD_RESPONSE_TIMEOUT : IFD_COMMUNICATION_ERROR;
	}
	if (apdu.rx_len > *RxLength) {
		*RxLength = 0;
		return IFD_COMMUNICATION_ERROR;
	}
	memcpy(RxBuffer, apdu.rx, apdu.rx_len);
	*RxLength = apdu.rx_len;
	if (RecvPci) {
		RecvPci->Protocol = SendPci.Protocol;
		RecvPci->Length = sizeof(SCARD_IO_HEADER);
	}
	return IFD_SUCCESS;
}

RESPONSECODE IFDHControl(DWORD Lun, DWORD ControlCode, PUCHAR TxBuffer, DWORD TxLength, PUCHAR RxBuffer, DWORD RxLength,
			 PDWORD pdwBytesReturned)
{
	*pdwBytesReturned = 0;
	return IFD_ERROR_NOT_SUPPORTED;
}

RESPONSECODE IFDHICCPresence(DWORD Lun)
{
	struct reader *r = reader_of(Lun);
	int present = 0;

	if (!r)
		return IFD_COMMUNICATION_ERROR;
	if (ioctl(r->fd, TORNE_CARD_IOC_PRESENT, &present) < 0)
		return IFD_COMMUNICATION_ERROR;
	return present ? IFD_ICC_PRESENT : IFD_ICC_NOT_PRESENT;
}
