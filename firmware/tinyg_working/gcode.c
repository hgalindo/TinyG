/*
 * gcode.c - rs274/ngc parser.
 * Part of Grbl
 * This code is inspired by the Arduino GCode Interpreter by Mike Ellery and the 
 * NIST RS274/NGC Interpreter by Kramer, Proctor and Messina. 
 *
 * Copyright (c) 2009 Simen Svale Skogsrud
 * Modified for TinyG project by Alden S Hart, Jr.
 *
 * Grbl is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation, 
 * either version 3 of the License, or (at your option) any later version.
 *
 * Grbl is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; 
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR 
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Grbl.  
 * If not, see <http://www.gnu.org/licenses/>.
 */
/* 
  Supported commands are:
 	G0				Rapid linear motion
	G1				Linear motion at feed rate
	G2, G3			Clockwise / counterclockwise arc at feed rate
	G4				Dwell
	G17, G18, G19	Select plane: XY plane {G17}, XZ plane {G18}, YZ plane {G19}
	G20, G21		Length units: inches {G20}, millimeters {G21}
	G53				Move in absolute coordinates
	G80				Cancel modal motion
	G90, G91		Set distance mode; absolute {G90}, incremental {G91}
	G93, G94		Set feed rate mode: inverse time mode {93}, 
										units per minute mode {G94}
	M0				Program stop
	M1				Optional program stop
	M2				Program end
	M3, M4			Turn spindle clockwise / counterclockwise
	M5				Stop spindle turning
	M30				Program end (pallet shuttle and reset)
	M60				Program stop (and pallet shuttle)

  Commands omitted for the time being:
	G10	  			Coordinate system data
	G14, G15		Spiral motion
	G28, G30		Return to home (requires parameters)
	G38.2 			Straight probe
	G40, G41, G42	Cutter radius compensation
	G43, G49		Tool length offsets
	G54 - G59.3		Select coordinate system (group 12)
	G61, G61.1, G64 Set path control mode (group 13)
	G81 - G89		Canned cycles
	G92	- G92.3		Coordinate system offsets
	G98, G99		Set canned cycle return level

	M6				Tool change
	M7, M8, M9		Coolant (group8)
	M48, M49		Enable/disable feed and speed override switches (group 9)
	
  Other commands and features intentionally not supported:
	- A,B,C axes
	- Multiple coordinate systems
	- Evaluation of expressions
	- Variables (Parameters)
	- Multiple home locations
	- Probing
	- Override control

  FYI: GCode modal groups (from NIST RS274NGC_3 Table 4)

   The modal groups for G codes are:
	group 1 = {G0, G1, G2, G3, G38.2, G80, G81, G82, G83, G84, G85, G86, G87, G88, G89} motion
	group 2 = {G17, G18, G19} plane selection 
	group 3 = {G90, G91} distance mode 
	group 5 = {G93, G94} feed rate mode
	group 6 = {G20, G21} units 
	group 7 = {G40, G41, G42} cutter radius compensation 
	group 8 = {G43, G49} tool length offset 
	group 10 = {G98, G99} return mode in canned cycles 
	group 12 = {G54, G55, G56, G57, G58, G59, G59.1, G59.2, G59.3} coordinate system selection 
	group 13 = {G61, G61.1, G64} path control mode

   The modal groups for M codes are:
	group 4 = {M0, M1, M2, M30, M60} stopping 
	group 6 = {M6} tool change 
	group 7 = {M3, M4, M5} spindle turning 
	group 8 = {M7, M8, M9} coolant (special case: M7 and M8 may be active at the same time) 
	group 9 = {M48, M49} enable/disable feed and speed override switches

   In addition to the above modal groups, there is a group for non-modal G codes:
	group 0 = {G4, G10, G28, G30, G53, G92, G92.1, G92.2, G92.3}	
*/

#include <ctype.h>
#include <stdlib.h>
#include <string.h>					// for memset()
#include <math.h>
#include <stdio.h>
#include <avr/pgmspace.h>			// needed for exception strings

#include "tinyg.h"
#include "gcode.h"					// must precede config.h
#include "config.h"
#include "controller.h"
#include "motion_control.h"
#include "spindle.h"

struct GCodeState {
	// parser variables
	uint8_t status;					// now uses unified TG_ status codes
	char letter;					// parsed letter, eg.g. G or X or Y
	double value;					// number parsed from line (e.g. 2 for G2
	double fraction;				// fractional value of number (e.g. 0.1 for 92.1)

	// model state
	uint8_t next_action; 			// "G mode" - preserved across blocks
	uint8_t program_flow;			// M0, M1 - pause / resume program flow
	uint8_t motion_mode;			// G0, G1, G2, G3, G38.2, G80, G81, G82, G83, 
									// ...G84, G85, G86, G87, G88, G89 (modal group 1)
	uint8_t inverse_feed_rate_mode; // G93, G94 (feed rate group; modal group 5)
	uint8_t inches_mode;         	// 0 = millimeter mode, 1 = inches mode {G21,G20}
	uint8_t absolute_mode;       	// 0 = relative motion, 1 = abs motion {G90,G91}
	uint8_t radius_mode;			// TRUE = radius mode
	uint8_t set_origin_mode;		// TRUE = in set origin mode {G92}
	uint8_t absolute_override; 		// TRUE=absolute motion for this block only{G53}

	uint8_t plane_axis_0; 			// axes of the selected plane
	uint8_t plane_axis_1; 
	uint8_t plane_axis_2; 

  	double dwell_time; 				// dwell time in seconds (was 'p' in older code)
	double radius;					// radius value
	double feed_rate; 				// millimeters/second
	double seek_rate;				// millimeters/second
	double unit_converted_value;
	double inverse_feed_rate; 		// negative inverse_feed_rate means 
									//    no inverse_feed_rate specified
	double position[3];				// where the interpreter considers the tool
	double target[3]; 				// where the move should go
	double offset[3];  				// used by arc commands

	uint8_t tool;
	int8_t spindle_direction;
	int16_t spindle_speed;			// RPM/100
};
static struct GCodeState gc;

/* local helper functions */
static void _gc_normalize_gcode_block(char *block);
static int _gc_read_double(char *buf, int *i, double *double_ptr);
static int _gc_next_statement(char *letter, double *value_ptr, double *fraction_ptr, char *line, int *i);
static int _gc_compute_arc(void);
static int _gc_compute_radius_arc(void);
static int _gc_compute_center_arc(void);

/* 
 * gc_init() 
 */

void gc_init() {
	memset(&gc, 0, sizeof(gc));				// must set doubles independently (true?)
  	gc.dwell_time = 0; 						// was 'p' 
	gc.radius = 0;							// radius value
	gc.feed_rate = cfg.default_feed_rate;	// Note: is divided by 60 in Grbl
	gc.seek_rate = cfg.default_seek_rate;	// Note: is divided by 60 in Grbl

	gc.inches_mode = TRUE;					// default for US
	gc.absolute_mode = TRUE;
	gc.inverse_feed_rate = -1; 				// negative inverse_feed_rate means 
											//	  no inverse_feed_rate specified
	gc.radius_mode = FALSE;
	gc.absolute_override = FALSE; 			// TRUE=absolute motion for this block only{G53}
	gc.next_action = NEXT_ACTION_NONE; 		// no operation

	select_plane(X, Y, Z);					// default planes, 0, 1 and 2
}

/*
 * gc_send_to_parser() - send a block of gcode to the parser
 *
 *	Inject a block into parser taking gcode command processing state into account
 */

void gc_send_to_parser(char *block)
{
	gc_gcode_parser(block);
	return;
}

/*
 * gc_gcode_parser() - parse a block (line) of gcode
 */

uint8_t gc_gcode_parser(char *block)
{
	_gc_normalize_gcode_block(block);
	if (block[0] == 0) { 					// ignore comments (stripped)
		return(TG_OK);
	}
	if (block[0] == 'Q') {					// quit gcode mode
		return(TG_QUIT);
	}
	gc.status = gc_execute_block(block);	// execute gcode block
	tg_print_status(gc.status, block);
	return (gc.status);
}

/*
 * _gc_normalize_gcode_block() - normalize a block (line) of gcode in place
 *
 *	Comments always terminate the block (embedded comments are not supported)
 *	Messages in comments are sent to console (stderr)
 *	Processing: split string into command and comment portions. Valid choices:
 *	  supported:	command
 *	  supported:	comment
 *	  supported:	command comment
 *	  unsupported:	command command
 *	  unsupported:	comment command
 *	  unsupported:	command comment command
 *
 *	Valid characters in a Gcode block are (see RS274NGC_3 Appendix E)
 *		digits						all digits are passed to interpreter
 *		lower case alpha			all alpha is passed
 *		upper case alpha			all alpha is passed
 *		+ - . / *	< = > 			chars passed to interpreter
 *		| % # ( ) [ ] { } 			chars passed to interpreter
 *		<sp> <tab> 					chars are legal but are not passed
 *		/  							if first, block delete char - omits the block
 *
 *	Invalid characters in a Gcode block are:
 *		control characters			chars < 0x20
 *		! $ % ,	; ; ? @ 
 *		^ _ ~ " ' <DEL>
 *
 *	MSG specifier in comment can have mixed case but cannot cannot have 
 *	embedded white spaces
 */

void _gc_normalize_gcode_block(char *block) 
{
	char c;
	char *comment=0;	// comment pointer - first char past opening paren
	uint8_t i=0; 		// index for incoming characters
	uint8_t j=0;		// index for normalized characters

	// discard deleted block
	if (block[0] == '/') {
		block[0] = 0;
		return;
	}
	// normalize the comamnd block & mark the comment(if any)
	while ((c = toupper(block[i++])) != 0) {// NUL character
		if ((isupper(c)) || (isdigit(c))) {	// capture common chars
		 	block[j++] = c; 
			continue;
		}
		if (c == '(') {						// detect & handle comments
			block[j] = 0;
			comment = &block[i]; 
			break;
		}
		if (c <= ' ') continue;				// toss controls & whitespace
		if (c == 0x7F) continue;			// toss DELETE
		if (strchr("!$%,;:?@^_~`\'\"", c))	// toss invalid punctuation
			continue;
		block[j++] = c;
	}
	block[j] = 0;							// nul terminate the command
	if (comment) {
		if ((toupper(comment[0]) == 'M') && 
			(toupper(comment[1]) == 'S') &&
			(toupper(comment[2]) == 'G')) {
			i=0;
			while ((c = comment[i++]) != 0) {// remove trailing parenthesis
				if (c == ')') {
					comment[--i] = 0;
					break;
				}
			}
			printf_P(PSTR("%s\n"), (comment+3));
		}
	}
}

/* 
 * select_plane() - select axis plane 
 */

void select_plane(uint8_t axis_0, uint8_t axis_1, uint8_t axis_2) 
{
	gc.plane_axis_0 = axis_0;
	gc.plane_axis_1 = axis_1;
	gc.plane_axis_2 = axis_2;
}

/*
 * to_millimeters()
 */

//inline float to_millimeters(double value) 	// inline won't compile at -O0
float to_millimeters(double value) 
{
	return(gc.inches_mode ? (value * INCHES_PER_MM) : value);
}

/* 
 * theta(double x, double y)
 *
 *	Find the angle in radians of deviance from the positive y axis. 
 *	negative angles to the left of y-axis, positive to the right.
 */

double theta(double x, double y)
{
	double theta = atan(x/fabs(y));

	if (y>0) {
		return(theta);
	} else {
		if (theta>0) 
	    {
			return(M_PI-theta);
    	} else {
			return(-M_PI-theta);
		}
	}
}

/* 
 * _gc_next_statement() - parse next block of Gcode
 *
 *	Parses the next statement and leaves the counter on the first character 
 *	following the statement. 
 *	Returns TRUE if there was a statement, FALSE if end of string was reached
 *	or there was an error (check gc.status).
 */

int _gc_next_statement(char *letter, double *value_ptr, 
					   double *fraction_ptr, char *buf, int *i) {
	if (buf[*i] == 0) {
		return(FALSE); // No more statements
	}
	*letter = buf[*i];
	if(!isupper(*letter)) {
		gc.status = TG_EXPECTED_COMMAND_LETTER;
		return(FALSE);
	}
	(*i)++;
	if (!_gc_read_double(buf, i, value_ptr)) {
		return(FALSE);
	};
	*fraction_ptr = (*value_ptr - trunc(*value_ptr));
	return(TRUE);
}

/* 
 * _gc_read_double() - read a double from a Gcode statement 
 *
 *	buf			string: line of RS274/NGC code being processed
 *	i			index into string array (position on the line)
 *	double_ptr	pointer to double to be read
 */

int _gc_read_double(char *buf, int *i, double *double_ptr) 
{
	char *start = buf + *i;
	char *end;
  
	*double_ptr = strtod(start, &end);
	if(end == start) { 
		gc.status = TG_BAD_NUMBER_FORMAT; 
		return(FALSE); 
	};
	*i = end - buf;
	return(TRUE);
}

/*
 * gc_execute_block() - executes one line of NULL terminated G-Code. 
 *
 *	The line is assumed to contain only uppercase characters and signed floats 
 *	(no whitespace).
 */

uint8_t gc_execute_block(char *buf) 
{
	int i = 0;  				// index into Gcode block buffer (buf)
  
	clear_vector(gc.target);
	clear_vector(gc.offset);

	gc.status = TG_OK;
	gc.set_origin_mode = 0;	// you are not in set origin mode unless you say you are

  // Pass 1: Commands
	while(_gc_next_statement(&gc.letter, &gc.value, &gc.fraction, buf, &i)) {
    	switch(gc.letter) {
			case 'G':
				switch((int)gc.value) {
					case 0:  { gc.next_action = NEXT_ACTION_MOTION;
							   gc.motion_mode = MOTION_MODE_RAPID_LINEAR; break; }

					case 1:  { gc.next_action = NEXT_ACTION_MOTION;
							   gc.motion_mode = MOTION_MODE_LINEAR; break; }

					case 2:  { gc.next_action = NEXT_ACTION_MOTION;
					           gc.motion_mode = MOTION_MODE_CW_ARC; break; }

					case 3:  { gc.next_action = NEXT_ACTION_MOTION;
					           gc.motion_mode = MOTION_MODE_CCW_ARC; break; }

					case 4:  { gc.next_action = NEXT_ACTION_DWELL; break; }

					case 17: { select_plane(X, Y, Z); break; }
					case 18: { select_plane(X, Z, Y); break; }
					case 19: { select_plane(Y, Z, X); break; }

					case 20: { gc.inches_mode = TRUE; break; }
					case 21: { gc.inches_mode = FALSE; break; }

					case 28: { gc.next_action = NEXT_ACTION_GO_HOME; break; }
					case 30: { gc.next_action = NEXT_ACTION_GO_HOME; break; }

					case 53: { gc.absolute_override = TRUE; break; }
					case 80: { gc.motion_mode = MOTION_MODE_CANCEL; break; }
					case 90: { gc.absolute_mode = TRUE; break; }
					case 91: { gc.absolute_mode = FALSE; break; }
					case 92: { gc.set_origin_mode = TRUE; break; }
					case 93: { gc.inverse_feed_rate_mode = TRUE; break; }
					case 94: { gc.inverse_feed_rate_mode = FALSE; break; }
					default: gc.status = TG_UNSUPPORTED_STATEMENT;
				}
				break;

			case 'M':
				switch((int)gc.value) {
					case 0: case 1: gc.program_flow = PROGRAM_FLOW_STOP; break;
					case 2: case 30: case 60: gc.program_flow = PROGRAM_FLOW_END; break;
					case 3: gc.spindle_direction = 1; break;
					case 4: gc.spindle_direction = -1; break;
					case 5: gc.spindle_direction = 0; break;
        			default: gc.status = TG_UNSUPPORTED_STATEMENT;
				}
				break;

			case 'T': gc.tool = trunc(gc.value); break;
		}
		if(gc.status) {
			break;
		}
	}
  
  // If there were any errors parsing this line return right away with the bad news
	if (gc.status) { 
		return(gc.status); 
	}

	i = 0;
	clear_vector(gc.offset);
	memcpy(gc.target, gc.position, sizeof(gc.target)); // target = position

  // Pass 2: Parameters
	while(_gc_next_statement(&gc.letter, &gc.value, &gc.fraction, buf, &i)) {
		gc.unit_converted_value = to_millimeters(gc.value);
		switch(gc.letter) {
			case 'F': 
				if (gc.inverse_feed_rate_mode) {
					gc.inverse_feed_rate = gc.unit_converted_value; // secs per motion for this motion only
				} else {
					gc.feed_rate = gc.unit_converted_value/60; // mm per second
				}
				break;
			case 'I': case 'J': case 'K': {
				gc.offset[gc.letter-'I'] = gc.unit_converted_value; 
				break;
			}
			case 'P': {
				gc.dwell_time = gc.value; 			// dwell time in seconds
				break;
			}
			case 'R': {
				gc.radius = gc.unit_converted_value; 
				gc.radius_mode = TRUE; 
				break;
			}
			case 'S': {
				gc.spindle_speed = gc.value; 
				break;
			}
			case 'X': case 'Y': case 'Z': {
				if (gc.set_origin_mode) {
					gc.position[gc.letter - 'X'] = gc.unit_converted_value;
					gc.target[gc.letter - 'X'] = gc.position[gc.letter - 'X'];
//					memcpy(gc.target, gc.position, sizeof(gc.target));	// target = position
					gc.next_action = NEXT_ACTION_SET_COORDINATES;
				} else if (gc.absolute_mode || gc.absolute_override) {
					gc.target[gc.letter - 'X'] = gc.unit_converted_value;
				} else {
					gc.target[gc.letter - 'X'] += gc.unit_converted_value;
				}
 				break;
			}
		}	
	}
  
  // If there were any errors parsing this line return right away with the bad news
  	if (gc.status) {
		return(gc.status); 
	}
    
  // Update spindle state
	if (gc.spindle_direction) {
    	sp_spindle_run(gc.spindle_direction, gc.spindle_speed);
	} else {
		sp_spindle_stop();
	}
  
  // Perform any physical actions
	switch (gc.next_action) {
		case NEXT_ACTION_NONE: {				// nothing to do here
			break;
		}

		case NEXT_ACTION_GO_HOME: { 
			gc.status = mc_home(); 
			break;
		}

		case NEXT_ACTION_SET_COORDINATES: { 
			gc.status = mc_set_position(gc.position[X], gc.position[Y], gc.position[Z]); 
			break;
		}

		case NEXT_ACTION_DWELL: {
			gc.status = mc_dwell(gc.dwell_time); 
			break;
		}

		case NEXT_ACTION_MOTION: {
			switch (gc.motion_mode) {
				case MOTION_MODE_CANCEL: {
					break;
				}

				case MOTION_MODE_RAPID_LINEAR: case MOTION_MODE_LINEAR: {
					gc.status = mc_line(gc.target[X], gc.target[Y], gc.target[Z],
							    	   (gc.inverse_feed_rate_mode) ? gc.inverse_feed_rate : gc.feed_rate,
										gc.inverse_feed_rate_mode); 
					break;
				}

				case MOTION_MODE_CW_ARC: case MOTION_MODE_CCW_ARC: {
					gc.status = _gc_compute_arc(); 
					break;
				}
			}
		}
	}
	/* As far as the g-code parser is concerned the position is now == target. 
	 * In reality, motion control / steppers will still be processing the action
	 * and the real tool position is still close to the starting point.
	 * The endpoint position is not moved if there has been an interpreter error.
	 */
	if ((gc.status == TG_OK) || (gc.status == TG_EAGAIN)) {
		memcpy(gc.position, gc.target, sizeof(gc.target));
	};
	return(gc.status);
}


/*
 * _gc_compute_arc() - arc computation helper routine 
 */

int _gc_compute_arc()
{
	if (gc.radius_mode) {
		if ((_gc_compute_radius_arc() != TG_OK)) {
			return (gc.status);
		}
	}
	return (_gc_compute_center_arc());
}

/*
 * _gc_compute_radius_arc()
 *
 * Compute arc center (offset) from radius. Used to prep for computing an center arc
 */

int _gc_compute_radius_arc()
{
	double x;
	double y;
	double h_x2_div_d;

/*  We need to calculate the center of the circle that has the designated 
	radius and passes through both the current position and the target position
		  
	This method calculates the following set of equations where:
	`  [x,y] is the vector from current to target position, 
		d == magnitude of that vector, 
		h == hypotenuse of the triangle formed by the radius of the circle, 
			 the distance to the center of the travel vector. 
		  
	A vector perpendicular to the travel vector [-y,x] is scaled to the length
	of h [-y/d*h, x/d*h] and added to the center of the travel vector [x/2,y/2]
	to form the new point [i,j] at [x/2-y/d*h, y/2+x/d*h] which will be the 
	center of our arc.
          
       d^2 == x^2 + y^2
       h^2 == r^2 - (d/2)^2
       i == x/2 - y/d*h
       j == y/2 + x/d*h
                                                          O <- [i,j]
                                            -  |
                                  r      -     |
                                      -        |
                                   -           | h
                                -              |
                  [0,0] ->  C -----------------+--------------- T  <- [x,y]
                            | <------ d/2 ---->|
                    
       C - Current position
       T - Target position
       O - center of circle that pass through both C and T
       d - distance from C to T
       r - designated radius
       h - distance from center of CT to O
          
	Expanding the equations:

      	d -> sqrt(x^2 + y^2)
        h -> sqrt(4 * r^2 - x^2 - y^2)/2
        i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2 
        j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2)) / sqrt(x^2 + y^2)) / 2
         
	Which can be written:
          
        i -> (x - (y * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
        j -> (y + (x * sqrt(4 * r^2 - x^2 - y^2))/sqrt(x^2 + y^2))/2
          
	Which we for size and speed reasons optimize to:

       	h_x2_div_d = sqrt(4 * r^2 - x^2 - y^2)/sqrt(x^2 + y^2)
      	i = (x - (y * h_x2_div_d))/2
      	j = (y + (x * h_x2_div_d))/2  
	*/
        
	// Calculate the change in position along each selected axis
	x = gc.target[gc.plane_axis_0]-gc.position[gc.plane_axis_0];
	y = gc.target[gc.plane_axis_1]-gc.position[gc.plane_axis_1];
        
	clear_vector(&gc.offset);
	 // == -(h * 2 / d)
	h_x2_div_d = -sqrt(4 * gc.radius*gc.radius - ((x*x) - (y*y))) / hypot(x,y);

	// If r is smaller than d the arc is now traversing the complex plane beyond
	// the reach of any real CNC, and thus - for practical reasons - we will 
	// terminate promptly (well spoken Simen!)
	if(isnan(h_x2_div_d)) { 
		gc.status = TG_FLOATING_POINT_ERROR; 
		return(gc.status); 
	}

	// Invert the sign of h_x2_div_d if circle is counter clockwise 
	// (see sketch below)
	if (gc.motion_mode == MOTION_MODE_CCW_ARC) {
		h_x2_div_d = -h_x2_div_d;
	}

	/*	The counter clockwise circle lies to the left of the target direction. 
		When offset is positive, the left hand circle will be generated - 
		when it is negative the right hand circle is generated.

    
                                     T  <-- Target position
    
                                     ^ 
        Clockwise circles with       |     Clockwise circles with
		this center will have        |     this center will have
        > 180 deg of angular travel  |     < 180 deg of angular travel, 
                          \          |      which is a good thing!
                           \         |         /
    center of arc when  ->  x <----- | -----> x <- center of arc when 
    h_x2_div_d is positive           |             h_x2_div_d is negative
                                     |
    
                                     C  <-- Current position
	*/                

	// Negative R is g-code-alese for "I want a circle with more than 180 degrees
	// of travel" (go figure!), even though it is advised against ever generating
	// such circles in a single line of g-code. By inverting the sign of 
	// h_x2_div_d the center of the circles is placed on the opposite side of 
	// the line of travel and thus we get the unadvisably long arcs as prescribed.
	if (gc.radius < 0) { 
		h_x2_div_d = -h_x2_div_d; 
	}        
        
	// Complete the operation by calculating the actual center of the arc
	gc.offset[gc.plane_axis_0] = (x-(y*h_x2_div_d))/2;
	gc.offset[gc.plane_axis_1] = (y+(x*h_x2_div_d))/2;
	return (gc.status);
} 
    
/*
 * _gc_compute_center_arc()
 *
 * Compute the arc move given I and J (arc center point - found in offset vector).
 */

int _gc_compute_center_arc()
{
	double theta_start;
	double theta_end;
	double angular_travel;
	double radius_tmp;
	double depth;

    /*	This segment sets up an clockwise or counterclockwise arc from the current
		position to the target position around the center designated by the offset
		vector. All theta-values measured in radians of deviance from the positive 
		y-axis. 

                        | <- theta == 0
                      * * *
                    *       *
                  *           *
                  *     O ----T   <- theta_end (e.g. 90 degrees: theta_end == PI/2)
                  *   /
                    C   <- theta_start (e.g. -145 degrees: theta_start == -PI*(3/4))
 	*/

	// calculate the theta (angle) of the current point
	theta_start = theta(-gc.offset[gc.plane_axis_0], -gc.offset[gc.plane_axis_1]);
	if(isnan(theta_start)) { 
		gc.status = TG_ARC_SPECIFICATION_ERROR;
		return(gc.status); 
	}

	// calculate the theta (angle) of the target point
	theta_end = theta(gc.target[gc.plane_axis_0] 
					- gc.offset[gc.plane_axis_0] 
					- gc.position[gc.plane_axis_0], 
 					  gc.target[gc.plane_axis_1] 
					- gc.offset[gc.plane_axis_1] 
					- gc.position[gc.plane_axis_1]);

	if(isnan(theta_end)) { 
		gc.status = TG_ARC_SPECIFICATION_ERROR; 
		return(gc.status);
	}

	// ensure that the difference is positive so that we have clockwise travel
	if (theta_end < theta_start) {
		theta_end += 2*M_PI;
	}
	angular_travel = theta_end - theta_start;

	// Invert angular motion if the g-code wanted a counterclockwise arc
	if (gc.motion_mode == MOTION_MODE_CCW_ARC) {
		angular_travel = angular_travel - 2*M_PI;
	}

	// Find the radius
	radius_tmp = hypot(gc.offset[gc.plane_axis_0], gc.offset[gc.plane_axis_1]);

	// Calculate the motion along the depth axis of the helix
	depth = gc.target[gc.plane_axis_2] - gc.position[gc.plane_axis_2];

	// Trace the arc
	gc.status = mc_arc(theta_start, angular_travel, radius_tmp, depth, 
					   gc.plane_axis_0, gc.plane_axis_1, gc.plane_axis_2, 
        	   		  (gc.inverse_feed_rate_mode) ? gc.inverse_feed_rate : gc.feed_rate, 
					   gc.inverse_feed_rate_mode);

    // Finish off with a line to make sure we arrive exactly where we think we are
	//--> For this to work correctly it must be delivered ONLY after the arc generator 
	// has completed the arc. So the endpoint should be passed to the generator and
	// executed there.
//	gc.status = mc_line(gc.target[X_AXIS], gc.target[Y_AXIS], gc.target[Z_AXIS], 
//					   (gc.inverse_feed_rate_mode) ? gc.inverse_feed_rate : 
//						gc.feed_rate, gc.inverse_feed_rate_mode);
	return (gc.status);
}

