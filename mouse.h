// API for mouse management
#include "date.h"

#define MOUSE_LEFT_BUTTON 0
#define MOUSE_RIGHT_BUTTON 1

// Maximum size of the event list
#define MOUSE_MAX_EVENTS 4

typedef struct {
	int button;
	int x;
	int y;
	struct rtcdate timestamp;
} mouse_event_t;

typedef struct {
	// actual position of mouse
	int x;
	int y;
	// event list
	mouse_event_t events[MOUSE_MAX_EVENTS];
	int n_events;
} mouse_state_t;

// returns a mouse_state_t with the current position of the mouse
// and an EMPTY list of events
mouse_state_t mouse_get_position(void);

// returns mouse current state (current state + list of unhandled
// mouse events)
mouse_state_t mouse_get_state(void);
