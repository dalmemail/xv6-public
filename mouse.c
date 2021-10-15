// Mouse driver
#include "traps.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "mouse.h"
#include "spinlock.h"
#include "sleeplock.h" // we need this one before including file.h
#include "fs.h" // and this one
#include "file.h"

#define DATA_IN_BUFFER 0x01

#define ACK 0xFA

// mouse current position
static int pos_x;
static int pos_y;

// position 0 is for the kernel API
// position 1 is for userland (access through device file)
static mouse_event_t events[2][MOUSE_MAX_EVENTS];
static unsigned int n_events[2];

static struct spinlock mouselock;
static struct spinlock mouse_device_file_lock;

// These matches for the syscalls read() and write()
int mouse_read(struct inode *ip, char *dst, int n);
int mouse_write(struct inode *ip, char *buf, int n);

// We wait for ACKNOWLEDGED, with a timeout
// returns 0 on success, -1 if timeout was reached
static int _wait_for_ACK()
{
	int i = 0;

	// while we can't read data from 0x60, or data from 0x60 is
	// not ACK
	while (!(inb(0x64) & DATA_IN_BUFFER) || (inb(0x60) != ACK)) {
		i++;
		if (i == 100000)
			return -1;
	}

	return 0;
}

// wait for the input buffer to be available to write on it
static int _wait_for_output_buffer()
{
	int i = 0;

	// while we can't write to the input buffer..
	while (inb(0x64) & 2) {
		i++;
		if (i == 100000)
			return -1;
	}

	return 0;
}

// wait until the controller has data for us
static int _wait_for_data()
{
	int i = 0;

	// while we can't read data from 0x60, or data from 0x60 is
	// not ACK
	while (!(inb(0x64) & DATA_IN_BUFFER)) {
		i++;
		if (i == 100000)
			return -1;
	}

	return 0;
}

void
mouseinit(void)
{
	if (_wait_for_output_buffer() < 0)
		return;

	// enable the mouse to send packages
	outb(0x64, 0xD4);
	if (_wait_for_output_buffer() < 0)
		return;
	outb(0x60, 0xF4);

	if (_wait_for_ACK() < 0)
		// failure on initialization
		return;

	if (_wait_for_output_buffer() < 0)
		return;

	outb(0x64, 0xD4);
	if (_wait_for_output_buffer() < 0)
		return;

	// tell the mouse we want to set the sample rate
	outb(0x60, 0xF3);
	if (_wait_for_ACK() < 0 || _wait_for_output_buffer() < 0)
		return;

	outb(0x64, 0xD4);
	if (_wait_for_output_buffer() < 0)
		return;

	outb(0x60, 40);
	if (_wait_for_ACK() < 0)
		return;

	// bit 1 of status register (0x64) must be clear
	// before writing to it, or to 0x60
	if (_wait_for_output_buffer() < 0)
		return;
	// command 0x20 asks for the PS/2 Controller Configuration Byte
	outb(0x64, 0x20);
	if (_wait_for_data() < 0)
		return;

	unsigned char res = inb(0x60);
	// we enable the PS/2 mouse interrupts
	res |= 1 << 1;
	// tell the controller we want to write the PS/2 Contr. Config. Byte
	outb(0x64, 0x60);
	if (_wait_for_output_buffer() < 0)
		return;

	outb(0x60, res);
	initlock(&mouselock, "mouse");
	initlock(&mouse_device_file_lock, "mouse");
	devsw[MOUSE].write = mouse_write;
	devsw[MOUSE].read = mouse_read;
	// CPU 0 handles mouse interrupts
	ioapicenable(IRQ_MOUSE, 0);
}

// must be called with mouselock locked
static void
_add_event(int button, int list_n)
{
	// if full we remove the oldest event
	if (n_events[list_n] == MOUSE_MAX_EVENTS) {
		// this assumes that MAX_EVENTS >= 1
		for (int i = 0; i+1 < MOUSE_MAX_EVENTS; i++)
			events[list_n][i] = events[list_n][i+1];

		n_events[list_n]--;
	}

	events[list_n][n_events[list_n]].button = button;
	events[list_n][n_events[list_n]].x = pos_x;
	events[list_n][n_events[list_n]].y = pos_y;
	cmostime(&(events[list_n][n_events[list_n]].timestamp));

	n_events[list_n]++;
}

// Interrupt handler
void
mouseintr(void)
{
	int count = 0;
	// we wait for data coming from the mouse (inb(0x64) & 0x20)
	while (!((inb(0x64) & DATA_IN_BUFFER) && (inb(0x64) & 0x20))) {
		count++;
		if (count == 1000)
			// discard package
			return;
	}
	count = 0;
	unsigned char byte1 = inb(0x60);
	while (!((inb(0x64) & DATA_IN_BUFFER) && (inb(0x64) & 0x20))) {
		count++;
		if (count == 1000)
			return;
	}
	unsigned char byte2 = inb(0x60);
	count = 0;
	while (!((inb(0x64) & DATA_IN_BUFFER) && (inb(0x64) & 0x20))) {
		count++;
		if (count == 1000)
			return;
	}
	unsigned char byte3 = inb(0x60);

	acquire(&mouselock);

	int x = (byte1 & 16) ? byte2 - 256 : byte2;
	int y = (byte1 & 32) ? byte3 - 256 : byte3;
	pos_x += x;
	pos_y += y;

	if (byte1 & 1)
		_add_event(MOUSE_LEFT_BUTTON, 0);
	else if (byte1 & 2)
		_add_event(MOUSE_RIGHT_BUTTON, 0);

	release(&mouselock);

	acquire(&mouse_device_file_lock);

	if (byte1 & 1)
		_add_event(MOUSE_LEFT_BUTTON, 1);
	else if (byte1 & 2)
		_add_event(MOUSE_RIGHT_BUTTON, 1);

	release(&mouse_device_file_lock);
}

mouse_state_t
mouse_get_position(void)
{
	acquire(&mouselock);
	// keep in mind that { } is not a valid initializer
	// according to C standard (although allowed by GCC)
	mouse_state_t res;
	res.x = pos_x;
	res.y = pos_y;
	res.n_events = 0;
	release(&mouselock);
	return res;
}

mouse_state_t
mouse_get_state(void)
{
	acquire(&mouselock);
	mouse_state_t res;
	res.x = pos_x;
	res.y = pos_y;
	res.n_events = n_events[0];
	if (n_events[0]) {
		for (int i = 0; i < n_events[0]; i++)
			res.events[i] = events[0][i];
	}

	n_events[0] = 0;
	release(&mouselock);
	return res;
}

static char msg[256];
static char number[16];
static int msg_pos = -1;
static char event_request = 0;

// this code is taken from printint(), at console.c
static int
_itoa(unsigned int x)
{
  static char digits[] = "0123456789";
  int i = 0;
  do{
    number[i++] = digits[x % 10];
  }while((x /= 10) != 0);
  return i-1;
}

static int
_put_backwards(int i, int j)
{
	for (; j >= 0; j--)
		msg[i++] = number[j];

	return i;
}

int
mouse_read(struct inode *ip, char *dst, int n)
{
	acquire(&mouselock);
	int _pos_x = pos_x;
	int _pos_y = pos_y;
	release(&mouselock);

	acquire(&mouse_device_file_lock);
	if (event_request && n_events[1]) {
		_pos_x = events[1][0].x;
		_pos_y = events[1][0].y;
	}
	// -1 means that there is no request being
	// handled, so we start a new one
	if (msg_pos == -1) {
		msg[0] = 'X';
		int i = 1;
		if (_pos_x < 0)
			msg[i++] = '-';

		i = _put_backwards(i, _itoa((_pos_x >= 0) ? _pos_x : -_pos_x));

		msg[i++] = 'Y';

		if (_pos_y < 0)
			msg[i++] = '-';

		i = _put_backwards(i, _itoa((_pos_y >= 0) ? _pos_y : -_pos_y));

		msg[i] = 0;
		msg_pos = 0;

		if (event_request) {
			event_request = 0;
			if (n_events[1]) {
				if (events[1][0].button == MOUSE_LEFT_BUTTON)
					msg[i++] = 'L';
				else
					msg[i++] = 'R';

				i = _put_backwards(i, _itoa(events[1][0].timestamp.year));
				msg[i++] = '/';
				i = _put_backwards(i, _itoa(events[1][0].timestamp.month));
				msg[i++] = '/';
				i = _put_backwards(i, _itoa(events[1][0].timestamp.day));
				msg[i++] = '@';
				i = _put_backwards(i, _itoa(events[1][0].timestamp.hour));
				msg[i++] = ':';
				i = _put_backwards(i, _itoa(events[1][0].timestamp.minute));
				msg[i++] = ':';
				i = _put_backwards(i, _itoa(events[1][0].timestamp.second));
				msg[i] = 0;

				// we remove the event
				for (int i = 0; i+1 < n_events[1]; i++) {
					events[1][i] = events[1][i+1];
				}
				n_events[1]--;
			}
		}
	}
	else if (!msg[msg_pos]) {
		// this request is ended
		msg_pos = -1;
		release(&mouse_device_file_lock);
		return 0;
	}

	int count = 0;
	while (n-- && msg[msg_pos])
		dst[count++] = msg[msg_pos++];

	release(&mouse_device_file_lock);

	return count;
}

int
mouse_write(struct inode *ip, char *buf, int n)
{
	if (n == 5 && !strncmp("EVENT", buf, 5)) {
		acquire(&mouse_device_file_lock);
		event_request = 1;
		release(&mouse_device_file_lock);
		return 5;
	}

	return -1;
}
