#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <random>
#include "ftd3xx.h"

using namespace std;

static bool do_exit;
static bool loop_mode;
static const uint32_t WR_CTRL_INTERVAL = 1000; /* 1 second */
static const uint32_t RD_CTRL_INTERVAL = 1000; /* 1 second */
static atomic_int tx_count;
static atomic_int rx_count;
static uint8_t ch_cnt;
static const int BUFFER_LEN = 128*1024;
static random_device rd;
static mt19937 rng(rd());
static uniform_int_distribution<size_t> random_len(1, BUFFER_LEN / 4);
static size_t file_length;
static bool transfer_failed;

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

static void stream_out(FT_HANDLE handle, uint8_t channel,
		string from)
{
	unique_ptr<uint8_t[]> buf(new uint8_t[BUFFER_LEN]);
	ifstream src;
	try {
		src.open(from, ios::binary);
	} catch (istream::failure e) {
		cout << "Failed to open file " << e.what() << endl;
		return;
	}
	size_t total = 0;

	while (!do_exit && total < file_length) {
		size_t len = random_len(rng) * 4;
		ULONG count = 0;

		src.read((char*)buf.get(), len);
		if (!src)
			len = (int)src.gcount();
		if (len) {
_retry:
			FT_STATUS status = FT_WritePipeEx(handle, channel, buf.get(),
						len, &count, WR_CTRL_INTERVAL + 100);
			if (FT_OK != status) {
				if (do_exit)
					break;
				printf("Channel %d failed to write %zu, ret %d\r\n", channel, total, status);
				if (FT_TIMEOUT == status)
					goto _retry;
				do_exit = true;
			}
		} else
			count = 0;
		tx_count += count;
		total += count;
	}
	src.close();
	printf("Channel %d write stopped, %zu\r\n", channel, total);
}

static void stream_in(FT_HANDLE handle, uint8_t channel,
		string to)
{
	unique_ptr<uint8_t[]> buf(new uint8_t[BUFFER_LEN]);
	ofstream dest;
	size_t total = 0;

	try {
		dest.open(to, ofstream::binary | ofstream::in | ofstream::out |
				ofstream::trunc);
	} catch (istream::failure e) {
		cout << "Failed to open file " << e.what() << endl;
		return;
	}

	while (!do_exit && total < file_length) {
		ULONG count = 0;
		size_t len = random_len(rng) * 4;
		size_t left = file_length - total;

		if (len > left)
			len = left;

		FT_STATUS status = FT_ReadPipeEx(handle, channel, buf.get(), len,
				&count, RD_CTRL_INTERVAL + 100);
		if (!count) {
			printf("Failed to read from channel %d, status:%d\r\n",
					channel, status);
			continue;
		}
		dest.write((const char *)buf.get(), count);
		rx_count += count;
		total += count;
	}
	dest.close();
	printf("Channel %d read stopped, %zu\r\n", channel, total);
}

static void sig_hdlr(int signum)
{
	switch (signum) {
	case SIGINT:
		do_exit = true;
		break;
	}
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

#if defined(_WIN32) || defined(_WIN64)
#define turn_off_all_pipes()
#define turn_off_thread_safe()
#else /* _WIN32 || _WIN64 */
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

#endif /* !_WIN32 && !_WIN64 */

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

	if (ch_cnt < 2)
		ch = CONFIGURATION_CHANNEL_CONFIG_1;
	else if (ch_cnt == 2)
		ch = CONFIGURATION_CHANNEL_CONFIG_2;
	else if (ch_cnt == 4)
		ch = CONFIGURATION_CHANNEL_CONFIG_4;
	else
		exit(-1);

	if (cfg->ChannelConfig == ch && current_is_600mode == is_600_mode &&
			!needs_update)
		return false;
	cfg->ChannelConfig = ch;
	cfg->FIFOClock = clock;
	cfg->FIFOMode = is_600_mode ? CONFIGURATION_FIFO_MODE_600 :
		CONFIGURATION_FIFO_MODE_245;
	return true;
}

static bool set_channel_config(CONFIGURATION_FIFO_CLK clock)
{
	FT_HANDLE handle;
	bool is_600_mode = ch_cnt != 0;
	DWORD dwType;

	/* Must turn off all pipes before changing chip configuration */
	turn_off_all_pipes();

	// TODO: FT603
	FT_GetDeviceInfoDetail(0, NULL, &dwType, NULL, NULL, NULL, NULL, &handle);
	if (!handle)
		return false;

	get_vid_pid(handle);

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
	printf("File transfer through FT245 loopback FPGA\r\n");
	printf("Usage: %s <src> <dest> <mode> [loop]\r\n", bin);
	printf("  src: source file name to read\r\n");
	printf("  dest: target file name to write\r\n");
	printf("  mode: 0 = FT245 mode(default), 1-4 FT600 channel count\r\n");
	printf("  loop: 0 = oneshot(default), 1 =  loop forever\r\n");
}

static bool validate_arguments(int argc, char *argv[])
{
	if (argc != 4 && argc != 5)
		return false;

	if (argc == 5) {
		int val = atoi(argv[4]);
		if (val != 0 && val != 1)
			return false;
		loop_mode = (bool)val;
	}

	ch_cnt = atoi(argv[3]);
	if (ch_cnt > 4)
		return false;
	return true;
}

static inline ifstream::pos_type get_file_length(ifstream &stream)
{
	return stream.seekg(0, ifstream::end).tellg();
}

static inline ifstream::pos_type get_file_length(const string &name)
{
	ifstream file(name, std::ifstream::binary);

	return get_file_length(file);
}

static bool compare_content(const string &from, const string &to)
{
	ifstream in1(from);
	ifstream in2(to);
	ifstream::pos_type size1, size2;

	size1 = get_file_length(in1);
	in1.seekg(0, ifstream::beg);

	size2 = get_file_length(in2);
	in2.seekg(0, ifstream::beg);

	if(size1 != size2) {
		cout << to << " size not same: " << size1 << " " << size2 << endl;
		in1.close();
		in2.close();
		return false;
	}

	static const size_t BLOCKSIZE = 4096;
	size_t remaining = size1;

	while(remaining) {
		char buffer1[BLOCKSIZE], buffer2[BLOCKSIZE];
		size_t size = std::min(BLOCKSIZE, remaining);

		in1.read(buffer1, size);
		in2.read(buffer2, size);

		if(0 != memcmp(buffer1, buffer2, size)) {
			for (size_t i = 0; i < size; i++) {
				if (buffer1[i] != buffer2[i]) {
					size_t offset = (int)size1 - (int)remaining + i;
					cout << to << " content not same at " << offset << endl;
					break;
				}
			}
			in1.close();
			in2.close();
			return false;
		}

		remaining -= size;
	}
	in1.close();
	in2.close();
	cout << to << " binary same" << endl;
	return true;
}

void file_transfer(FT_HANDLE handle, uint8_t channel, string from, string to)
{
	do {
		thread write_thread = thread(stream_out, handle, channel, from);
		thread read_thread = thread(stream_in, handle, channel, to);

		if (write_thread.joinable())
			write_thread.join();
		if (read_thread.joinable())
			read_thread.join();

		if (!compare_content(from, to))
			transfer_failed = true;
	} while (loop_mode && !do_exit);
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

	bool rev_a_chip = set_channel_config(CONFIGURATION_FIFO_CLK_100);

	/* Must be called before FT_Create is called */
	turn_off_thread_safe();

	FT_HANDLE handle;

	FT_Create(0, FT_OPEN_BY_INDEX, &handle);

	if (!handle) {
		printf("Failed to create device\r\n");
		return -1;
	}
	register_signals();

	if (ch_cnt == 0)
		ch_cnt = 1;

	for (int i = 0; i < ch_cnt; i++) {
		FT_SetPipeTimeout(handle, 2 + i, WR_CTRL_INTERVAL + 100);
		FT_SetPipeTimeout(handle, 0x82 + i, RD_CTRL_INTERVAL + 100);
	}

	thread transfer_thread[4];
	thread measure_thread = thread(show_throughput, handle);

	string from(argv[1]);
	string to(argv[2]);

	file_length = get_file_length(from);

	if (file_length == 0) {
		cout << "Input file not correct" << endl;
		return -1;
	}

	for (int i = 0; i < ch_cnt; i++) {
		string target = to;
		if (ch_cnt > 1)
			target += to_string(i);
		transfer_thread[i] = thread(file_transfer, handle, i, from, target);
	}

	for (int i = 0; i < ch_cnt; i++)
		transfer_thread[i].join();

	do_exit = true;
	if (measure_thread.joinable())
		measure_thread.join();

	/* Workaround for FT600/FT601 Rev.A device: Stop session before exit */
	if (rev_a_chip)
		FT_ResetDevicePort(handle);
	FT_Close(handle);
	return transfer_failed;
}
