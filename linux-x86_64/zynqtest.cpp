#include <iostream>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include "ftd3xx.h"

using namespace std;

static bool do_exit;
static bool fifo_600mode;
static atomic_int tx_count;
static atomic_int rx_count;
static uint8_t in_ch_cnt;
static uint8_t out_ch_cnt;
static thread measure_thread;
static thread write_thread;
static thread read_thread;
static const int BUFFER_LEN = 32*1024;

static void show_throughput(FT_HANDLE handle)
{
	auto next = chrono::steady_clock::now() + chrono::seconds(1);;
	(void)handle;

	while (!do_exit) {
		this_thread::sleep_until(next);
		next += chrono::seconds(1);

		int tx = tx_count.exchange(0);
		int rx = rx_count.exchange(0);

		printf("TX:%.2fMiB/s RX:%.2fMiB/s, total:%.2fMiB\r\n",
			(float)tx/1000/1000, (float)rx/1000/1000,
			(float)(tx+ rx)/1000/1000);
	}
}

static void write_test(FT_HANDLE handle)
{
	unique_ptr<uint8_t[]> buf(new uint8_t[BUFFER_LEN]);
    uint8_t *p_buf=buf.get();
    for(int i = 0; i<BUFFER_LEN; i++){
        p_buf[i]=/*(uint8_t(i/1024))*/i%256;
    }
	while (!do_exit) {
		for (uint8_t channel = 0; channel < out_ch_cnt; channel++) {
			ULONG count = 0;
            
			if (FT_OK != FT_WritePipeEx(handle, channel,
						buf.get(), BUFFER_LEN, &count, 1000)) {
				do_exit = true;
				break;
			}
            /*
            printf(" p_buf ----------------------------- \r\n");			
            for(int i = 0; i<BUFFER_LEN; i++){
                printf("%d ",p_buf[i]);
            }
            printf(" ----------------------------------- \r\n");
            printf("\r\n");
			*/
			tx_count += count;
		}
	}
	printf("Write stopped\r\n");
}

static void read_test(FT_HANDLE handle)
{
	unique_ptr<uint8_t[]> buf(new uint8_t[BUFFER_LEN]);
	const char *pBuf = (const char*)buf.get();
	ofstream dumpFile;

	dumpFile.open ("dumpfile.264", ios::out | ios::binary);
    
	while (!do_exit) {
		for (uint8_t channel = 0; channel < in_ch_cnt; channel++) {
			ULONG count = 0;
			if (FT_OK != FT_ReadPipeEx(handle, channel,
						buf.get(), BUFFER_LEN, &count, 1000)) {
				do_exit = true;
				break;
			}
            dumpFile.write(pBuf, count);
            //printf("dumpFile.write %d byte\r\n",count);

			rx_count += count;
		}
	}

	dumpFile.close();
	printf("Read stopped\r\n");
}

static void sig_hdlr(int signum)
{
	switch (signum) {
	case SIGINT:
		do_exit = true;
		break;
	}
}

static void test_gpio(HANDLE handle)
{
#define GPIO(x) (1 << (x))
#define GPIO_OUT(x) (1 << (x))
#define GPIO_HIGH(x) (1 << (x))

	DWORD dwMask = GPIO(0) | GPIO(1) | GPIO(2);
	DWORD dwDirection = GPIO_OUT(0) | GPIO_OUT(1) |GPIO_OUT(2);
	DWORD dwLevel = GPIO_HIGH(0) | GPIO_HIGH(1) |GPIO_HIGH(2);

	if (FT_NOT_SUPPORTED == FT_EnableGPIO(handle, dwMask, dwDirection)) {
		printf("FT_EnableGPIO not implemented\r\n");
		return;
	}
	if (FT_OK != FT_WriteGPIO(handle, dwMask, dwLevel)) {
		printf("FT_WriteGPIO not implemented\r\n");
		return;
	}
	printf("Change all GPIOs to output high\r\n");
	if (FT_OK != FT_ReadGPIO(handle, &dwLevel)) {
		printf("FT_ReadGPIO not implemented\r\n");
		return;
	}
	for (int i = 0; i < 3; i++)
		printf("GPIO%d level is %s\r\n", i,
				dwLevel & GPIO_HIGH(i) ? "high" : "low");
}

static void register_signals(void)
{
	signal(SIGINT, sig_hdlr);
}

static void get_version(void)
{
	DWORD dwVersion;

	FT_GetDriverVersion(NULL, &dwVersion);
	printf("Driver version:%d.%d.%d.%d\r\n", dwVersion >> 24,
			(uint8_t)(dwVersion >> 16), (uint8_t)(dwVersion >> 8),
			dwVersion & 0xFF);

	FT_GetLibraryVersion(&dwVersion);
	printf("Library version:%d.%d.%d\r\n", dwVersion >> 24,
			(uint8_t)(dwVersion >> 16), dwVersion & 0xFFFF);
}

static void get_vid_pid(FT_HANDLE handle)
{
	WORD vid, pid;

	if (FT_OK != FT_GetVIDPID(handle, &vid, &pid))
		return;
	printf("VID:%04X PID:%04X\r\n", vid, pid);
}

static void turn_off_all_pipes(void)
{
	FT_TRANSFER_CONF conf;

	memset(&conf, 0, sizeof(FT_TRANSFER_CONF));
	conf.wStructSize = sizeof(FT_TRANSFER_CONF);
	conf.pipe[FT_PIPE_DIR_IN].fPipeNotUsed = true;
	conf.pipe[FT_PIPE_DIR_OUT].fPipeNotUsed = true;
	for (DWORD i = 0; i < 4; i++)
		FT_SetTransferParams(&conf, i);
}

static bool get_device_lists(int timeout_ms)
{
	DWORD count;
	FT_DEVICE_LIST_INFO_NODE nodes[16];

	chrono::steady_clock::time_point const timeout =
		chrono::steady_clock::now() +
		chrono::milliseconds(timeout_ms);

	do {
		if (FT_OK == FT_CreateDeviceInfoList(&count))
			break;
		this_thread::sleep_for(chrono::microseconds(10));
	} while (chrono::steady_clock::now() < timeout);
	printf("Total %u device(s)\r\n", count);
	if (!count)
		return false;

	if (FT_OK != FT_GetDeviceInfoList(nodes, &count))
		return false;
	return true;
}


static bool set_ft600_channel_config(FT_60XCONFIGURATION *cfg,
		CONFIGURATION_FIFO_CLK clock, bool is_600_mode)
{
	bool needs_update = false;
	bool current_is_600mode;

	if (cfg->OptionalFeatureSupport &
			CONFIGURATION_OPTIONAL_FEATURE_ENABLENOTIFICATIONMESSAGE_INCHALL) {
		/* Notification in D3XX for Linux is implemented at OS level
		 * Turn off notification feature in firmware */
		cfg->OptionalFeatureSupport &=
			~CONFIGURATION_OPTIONAL_FEATURE_ENABLENOTIFICATIONMESSAGE_INCHALL;
		needs_update = true;
		printf("Turn off firmware notification feature\r\n");
	}

	if (!(cfg->OptionalFeatureSupport &
			CONFIGURATION_OPTIONAL_FEATURE_DISABLECANCELSESSIONUNDERRUN)) {
		/* Turn off feature not supported by D3XX for Linux */
		cfg->OptionalFeatureSupport |=
			CONFIGURATION_OPTIONAL_FEATURE_DISABLECANCELSESSIONUNDERRUN;
		needs_update = true;
		printf("disable cancel session on FIFO underrun 0x%X\r\n",
				cfg->OptionalFeatureSupport);
	}

	if (cfg->FIFOClock != clock)
		needs_update = true;

	if (cfg->FIFOMode == CONFIGURATION_FIFO_MODE_245) {
		printf("FIFO is running at FT245 mode\r\n");
		current_is_600mode = false;
	} else if (cfg->FIFOMode == CONFIGURATION_FIFO_MODE_600) {
		printf("FIFO is running at FT600 mode\r\n");
		current_is_600mode = true;
	} else {
		printf("FIFO is running at unknown mode\r\n");
		exit(-1);
	}

	UCHAR ch;

	if (in_ch_cnt == 1 && out_ch_cnt == 0)
		ch = CONFIGURATION_CHANNEL_CONFIG_1_INPIPE;
	else if (in_ch_cnt == 0 && out_ch_cnt == 1)
		ch = CONFIGURATION_CHANNEL_CONFIG_1_OUTPIPE;
	else {
		UCHAR total = in_ch_cnt < out_ch_cnt ? out_ch_cnt : in_ch_cnt;

		if (total == 4)
			ch = CONFIGURATION_CHANNEL_CONFIG_4;
		else if (total == 2)
			ch = CONFIGURATION_CHANNEL_CONFIG_2;
		else
			ch = CONFIGURATION_CHANNEL_CONFIG_1;

		if (cfg->FIFOMode == CONFIGURATION_FIFO_MODE_245 && total > 1) {
			printf("245 mode only support single channel\r\n");
			return false;
		}
	}

	if (cfg->ChannelConfig == ch && current_is_600mode == is_600_mode &&
			!needs_update)
		return false;
	cfg->ChannelConfig = ch;
	cfg->FIFOClock = clock;
	cfg->FIFOMode = is_600_mode ? CONFIGURATION_FIFO_MODE_600 :
		CONFIGURATION_FIFO_MODE_245;
	return true;
}


static bool set_channel_config(bool is_600_mode, CONFIGURATION_FIFO_CLK clock)
{
	FT_HANDLE handle;
	DWORD dwType;

	/* Must turn off all pipes before changing chip configuration */
	turn_off_all_pipes();

	FT_GetDeviceInfoDetail(0, NULL, &dwType, NULL, NULL, NULL, NULL, &handle);
	if (!handle)
		return false;

	get_vid_pid(handle);
	test_gpio(handle);

	union {
		FT_60XCONFIGURATION ft600;
	} cfg;
	if (FT_OK != FT_GetChipConfiguration(handle, &cfg)) {
		printf("Failed to get chip conf\r\n");
		return false;
	}

	bool needs_update;
		needs_update = set_ft600_channel_config(&cfg.ft600, clock, is_600_mode);
	if (needs_update) {
		if (FT_OK != FT_SetChipConfiguration(handle, &cfg))
			printf("Failed to set chip conf\r\n");
		else {
			printf("Configuration changed\r\n");
			this_thread::sleep_for(chrono::seconds(1));
			get_device_lists(6000);
		}
	}

	if (dwType == FT_DEVICE_600 || dwType == FT_DEVICE_601) {
		bool rev_a_chip;
		DWORD dwVersion;

		FT_GetFirmwareVersion(handle, &dwVersion);
		rev_a_chip = dwVersion <= 0x105;

		FT_Close(handle);
		return rev_a_chip;
	}

	FT_Close(handle);
	return false;
}

static void show_help(const char *bin)
{
	printf("Usage: %s <out channel count> <in channel count> [mode]\r\n", bin);
	printf("  channel count: [0, 1] for 245 mode, [0-4] for 600 mode\r\n");
	printf("  mode: 0 = FT245 mode (default), 1 = FT600 mode\r\n");
}

static void turn_off_thread_safe(void)
{
	FT_TRANSFER_CONF conf;

	memset(&conf, 0, sizeof(FT_TRANSFER_CONF));
	conf.wStructSize = sizeof(FT_TRANSFER_CONF);
	conf.pipe[FT_PIPE_DIR_IN].fNonThreadSafeTransfer = true;
	conf.pipe[FT_PIPE_DIR_OUT].fNonThreadSafeTransfer = true;
	for (DWORD i = 0; i < 4; i++)
		FT_SetTransferParams(&conf, i);
}

static void get_queue_status(HANDLE handle)
{
	for (uint8_t channel = 0; channel < out_ch_cnt; channel++) {
		DWORD dwBufferred;

		if (FT_OK != FT_GetUnsentBuffer(handle, channel,
					NULL, &dwBufferred)) {
			printf("Failed to get unsent buffer size\r\n");
			continue;
		}
		unique_ptr<uint8_t[]> p(new uint8_t[dwBufferred]);

		printf("CH%d OUT unsent buffer size in queue:%u\r\n",
				channel, dwBufferred);
		if (FT_OK != FT_GetUnsentBuffer(handle, channel,
					p.get(), &dwBufferred)) {
			printf("Failed to read unsent buffer size\r\n");
			continue;
		}
	}

	for (uint8_t channel = 0; channel < in_ch_cnt; channel++) {
		DWORD dwBufferred;

		if (FT_OK != FT_GetReadQueueStatus(handle, channel, &dwBufferred))
			continue;
		printf("CH%d IN unread buffer size in queue:%u\r\n",
				channel, dwBufferred);
	}
}

static bool validate_arguments(int argc, char *argv[])
{
	if (argc != 3 && argc != 4)
		return false;

	if (argc == 4) {
		int val = atoi(argv[3]);
		if (val != 0 && val != 1)
			return false;
		fifo_600mode = (bool)val;
	}

	out_ch_cnt = atoi(argv[1]);
	in_ch_cnt = atoi(argv[2]);

	if ((in_ch_cnt == 0 && out_ch_cnt == 0) ||
			in_ch_cnt > 4 || out_ch_cnt > 4) {
		show_help(argv[0]);
		return false;
	}
	return true;
}

int main(int argc, char *argv[])
{

	get_version();

	if (!validate_arguments(argc, argv)) {
		show_help(argv[0]);
		return 1;
	}

	if (!get_device_lists(500))
		return 1;

	bool rev_a_chip = set_channel_config(
			fifo_600mode, CONFIGURATION_FIFO_CLK_100);

	/* Must be called before FT_Create is called */
	turn_off_thread_safe();

	FT_HANDLE handle;

	FT_Create(0, FT_OPEN_BY_INDEX, &handle);

	if (!handle) {
		printf("Failed to create device\r\n");
		return -1;
	}
	
	 if (out_ch_cnt)
	 	write_thread = thread(write_test, handle);
	if (in_ch_cnt)
		read_thread = thread(read_test, handle);
	measure_thread = thread(show_throughput, handle);

	register_signals();

	if (write_thread.joinable())
	 	write_thread.join();
	if (read_thread.joinable())
		read_thread.join();

	if (measure_thread.joinable())
		measure_thread.join();
	get_queue_status(handle);

	/* Workaround for FT600/FT601 Rev.A device: Stop session before exit */
	if (rev_a_chip)
		FT_ResetDevicePort(handle);
	FT_Close(handle);
	return 0;
}
