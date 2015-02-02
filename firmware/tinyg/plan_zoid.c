/*
 * plan_zoid.c - acceleration managed line planning and motion execution - trapezoid planner
 * This file is part of the TinyG project
 *
 * Copyright (c) 2010 - 2014 Alden S. Hart, Jr.
 * Copyright (c) 2012 - 2014 Rob Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "tinyg.h"
#include "config.h"
#include "planner.h"
#include "report.h"
#include "util.h"
/*
#ifdef __cplusplus
extern "C"{
#endif
*/

float our_abs(const float number) { return number < 0 ? -number : number;}

/*
 * mp_calculate_trapezoid() - calculate trapezoid parameters
 *
 *	This rather brute-force and long-ish function sets section lengths and velocities
 *	based on the line length and velocities requested. It modifies the incoming
 *	bf buffer and returns accurate head, body and tail lengths, and accurate or
 *	reasonably approximate velocities. We care about accuracy on lengths, less
 *	so for velocity (as long as velocity err's on the side of too slow).
 *
 *	Note: We need the velocities to be set even for zero-length sections
 *	(Note: sections, not moves) so we can compute entry and exits for adjacent sections.
 *
 *	Inputs used are:
 *	  bf->length			- actual block length	(length is never changed)
 *	  bf->entry_velocity	- requested Ve			(entry velocity is never changed)
 *	  bf->cruise_velocity	- requested Vt			(is often changed)
 *	  bf->exit_velocity		- requested Vx			(may be changed for degenerate cases)
 *	  bf->cruise_vmax		- used in some comparisons
 *	  bf->delta_vmax		- used to degrade velocity of pathologically short blocks
 *
 *	Variables that may be set/updated are:
 *    bf->entry_velocity	- requested Ve
 *	  bf->cruise_velocity	- requested Vt
 *	  bf->exit_velocity		- requested Vx
 *	  bf->head_length		- bf->length allocated to head
 *	  bf->body_length		- bf->length allocated to body
 *	  bf->tail_length		- bf->length allocated to tail
 *
 *	Note: The following conditions must be met on entry:
 *		bf->length must be non-zero (filter these out upstream)
 *		bf->entry_velocity <= bf->cruise_velocity >= bf->exit_velocity
 */
/*	Classes of moves:
 *
 *	  Requested-Fit - The move has sufficient length to achieve the target velocity
 *		(cruise velocity). I.e: it will accommodate the acceleration / deceleration
 *		profile in the given length.
 *
 *	  Rate-Limited-Fit - The move does not have sufficient length to achieve target
 *		velocity. In this case the cruise velocity will be set lower than the requested
 *		velocity (incoming bf->cruise_velocity). The entry and exit velocities are satisfied.
 *
 *	  Degraded-Fit - The move does not have sufficient length to transition from
 *		the entry velocity to the exit velocity in the available length. These
 *		velocities are not negotiable, so a degraded solution is found.
 *
 *	  	In worst cases the move cannot be executed as the required execution time is
 *		less than the minimum segment time. The first degradation is to reduce the
 *		move to a body-only segment with an average velocity. If that still doesn't
 *		fit then the move velocity is reduced so it fits into a minimum segment.
 *		This will reduce the velocities in that region of the planner buffer as the
 *		moves are replanned to that worst-case move.
 *
 *	Various cases handled (H=head, B=body, T=tail)
 *
 *	  Requested-Fit cases
 *	  	HBT	Ve<Vt>Vx	sufficient length exists for all parts (corner case: HBT')
 *	  	HB	Ve<Vt=Vx	head accelerates to cruise - exits at full speed (corner case: H')
 *	  	BT	Ve=Vt>Vx	enter at full speed and decelerate (corner case: T')
 *	  	HT	Ve & Vx		perfect fit HT (very rare). May be symmetric or asymmetric
 *	  	H	Ve<Vx		perfect fit H (common, results from planning)
 *	  	T	Ve>Vx		perfect fit T (common, results from planning)
 *	  	B	Ve=Vt=Vx	Velocities are close to each other and within matching tolerance
 *
 *	  Rate-Limited cases - Ve and Vx can be satisfied but Vt cannot
 *	  	HT	(Ve=Vx)<Vt	symmetric case. Split the length and compute Vt.
 *	  	HT'	(Ve!=Vx)<Vt	asymmetric case. Find H and T by successive approximation.
 *		HBT'			body length < min body length - treated as an HT case
 *		H'				body length < min body length - subsume body into head length
 *		T'				body length < min body length - subsume body into tail length
 *
 *	  Degraded fit cases - line is too short to satisfy both Ve and Vx
 *	    H"	Ve<Vx		Ve is degraded (velocity step). Vx is met
 *	  	T"	Ve>Vx		Ve is degraded (velocity step). Vx is met
 *	  	B"	<short>		line is very short but drawable; is treated as a body only
 *		F	<too short>	force fit: This block is slowed down until it can be executed
 */
/*	NOTE: The order of the cases/tests in the code is pretty important. Start with the
 *	  shortest cases first and work up. Not only does this simplify the order of the tests,
 *	  but it reduces execution time when you need it most - when tons of pathologically
 *	  short Gcode blocks are being thrown at you.
 */

// The minimum lengths are dynamic and depend on the velocity
// These expressions evaluate to the minimum lengths for the current velocity settings
// Note: The head and tail lengths are 2 minimum segments, the body is 1 min segment
#define MIN_HEAD_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * (bf->cruise_velocity + bf->entry_velocity))
#define MIN_TAIL_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * (bf->cruise_velocity + bf->exit_velocity))
#define MIN_BODY_LENGTH (MIN_SEGMENT_TIME_PLUS_MARGIN * bf->cruise_velocity)

void mp_calculate_trapezoid(mpBuf_t *bf)
{
	//********************************************
	//********************************************
	//**   RULE #1 of mp_calculate_trapezoid()  **
	//**        DON'T CHANGE bf->length         **
	//********************************************
	//********************************************

	// F case: Block is too short - run time < minimum segment time
	// Force block into a single segment body with limited velocities
	// Accept the entry velocity, limit the cruise, and go for the best exit velocity
	// you can get given the delta_vmax (maximum velocity slew) supportable.

	bf->naiive_move_time = 2 * bf->length / (bf->entry_velocity + bf->exit_velocity); // average

	if (bf->naiive_move_time < MIN_SEGMENT_TIME_PLUS_MARGIN) {
		bf->cruise_velocity = bf->length / MIN_SEGMENT_TIME_PLUS_MARGIN;
		bf->exit_velocity = max(0.0, min(bf->cruise_velocity, (bf->entry_velocity - bf->delta_vmax)));
		bf->body_length = bf->length;
		bf->head_length = 0;
		bf->tail_length = 0;
		// We are violating the jerk value but since it's a single segment move we don't use it.
		return;
	}

	// B" case: Block is short, but fits into a single body segment

	if (bf->naiive_move_time <= NOM_SEGMENT_TIME) {
		bf->entry_velocity = bf->pv->exit_velocity;
		if (fp_NOT_ZERO(bf->entry_velocity)) {
			bf->cruise_velocity = bf->entry_velocity;
			bf->exit_velocity = bf->entry_velocity;
		} else {
			bf->cruise_velocity = bf->delta_vmax / 2;
			bf->exit_velocity = bf->delta_vmax;
		}
		bf->body_length = bf->length;
		bf->head_length = 0;
		bf->tail_length = 0;
		// We are violating the jerk value but since it's a single segment move we don't use it.
		return;
	}

	// B case:  Velocities all match (or close enough)
	//			This occurs frequently in normal gcode files with lots of short lines
	//			This case is not really necessary, but saves lots of processing time

	if (((bf->cruise_velocity - bf->entry_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) &&
	((bf->cruise_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE)) {
		bf->body_length = bf->length;
		bf->head_length = 0;
		bf->tail_length = 0;
		return;
	}

	// Head-only and tail-only short-line cases
	//	 H" and T" degraded-fit cases
	//	 H' and T' requested-fit cases where the body residual is less than MIN_BODY_LENGTH

	bf->body_length = 0;
	float minimum_length = mp_get_target_length(bf->entry_velocity, bf->exit_velocity, bf);
	if (bf->length <= (minimum_length + MIN_BODY_LENGTH)) {	// head-only & tail-only cases

		if (bf->entry_velocity > bf->exit_velocity)	{		// tail-only cases (short decelerations)
			if (bf->length < minimum_length) { 				// T" (degraded case)
				bf->entry_velocity = mp_get_target_velocity(bf->exit_velocity, bf->length, bf);
			}
			bf->cruise_velocity = bf->entry_velocity;
			bf->tail_length = bf->length;
			bf->head_length = 0;
			return;
		}

		if (bf->entry_velocity < bf->exit_velocity)	{		// head-only cases (short accelerations)
			if (bf->length < minimum_length) { 				// H" (degraded case)
				bf->exit_velocity = mp_get_target_velocity(bf->entry_velocity, bf->length, bf);
			}
			bf->cruise_velocity = bf->exit_velocity;
			bf->head_length = bf->length;
			bf->tail_length = 0;
			return;
		}
	}

	// Set head and tail lengths for evaluating the next cases
	bf->head_length = mp_get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
	bf->tail_length = mp_get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
	if (bf->head_length < MIN_HEAD_LENGTH) { bf->head_length = 0;}
	if (bf->tail_length < MIN_TAIL_LENGTH) { bf->tail_length = 0;}

	// Rate-limited HT and HT' cases
	if (bf->length < (bf->head_length + bf->tail_length)) { // it's rate limited

		// Symmetric rate-limited case (HT)
		if (fabs(bf->entry_velocity - bf->exit_velocity) < TRAPEZOID_VELOCITY_TOLERANCE) {
			bf->head_length = bf->length/2;
			bf->tail_length = bf->head_length;
			bf->cruise_velocity = min(bf->cruise_vmax, mp_get_target_velocity(bf->entry_velocity, bf->head_length, bf));

			if (bf->head_length < MIN_HEAD_LENGTH) {
				// Convert this to a body-only move
				bf->body_length = bf->length;
				bf->head_length = 0;
				bf->tail_length = 0;

				// Average the entry speed and computed best cruise-speed
				bf->cruise_velocity = (bf->entry_velocity + bf->cruise_velocity)/2;
				bf->entry_velocity = bf->cruise_velocity;
				bf->exit_velocity = bf->cruise_velocity;
			}
			return;
		}

		// Asymmetric HT' rate-limited case. This is relatively expensive but it's not called very often
		// iteration trap: uint8_t i=0;
		// iteration trap: if (++i > TRAPEZOID_ITERATION_MAX) { fprintf_P(stderr,PSTR("_calculate_trapezoid() failed to converge"));}
/*
        bf->cruise_velocity = mp_get_meet_velocity(bf->entry_velocity, bf->exit_velocity, bf->length, bf);
*/
		float computed_velocity = bf->cruise_vmax;
		uint8_t iterations = 0;
		do {
			bf->cruise_velocity = computed_velocity;	// initialize from previous iteration
			bf->head_length = mp_get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
			bf->tail_length = mp_get_target_length(bf->exit_velocity, bf->cruise_velocity, bf);
			if (bf->head_length > bf->tail_length) {
				bf->head_length = (bf->head_length / (bf->head_length + bf->tail_length)) * bf->length;
				if (++iterations < TRAPEZOID_ITERATION_MAX) {
					computed_velocity = mp_get_target_velocity(bf->entry_velocity, bf->head_length, bf);
				} else {
					computed_velocity = (computed_velocity + mp_get_target_velocity(bf->exit_velocity, bf->head_length, bf))/2;
					fprintf_P(stderr,PSTR("H"));
				}
			} else {
				bf->tail_length = (bf->tail_length / (bf->head_length + bf->tail_length)) * bf->length;
				if (++iterations < TRAPEZOID_ITERATION_MAX) {
					computed_velocity = mp_get_target_velocity(bf->exit_velocity, bf->tail_length, bf);
				} else {
					computed_velocity = (computed_velocity + mp_get_target_velocity(bf->exit_velocity, bf->tail_length, bf))/2;
				//	fprintf_P(stderr,PSTR("_calculate_trapezoid() failed to converge"));
					fprintf_P(stderr,PSTR("T"));
				}
			}
			// insert iteration trap here if needed
		} while ((fabs(bf->cruise_velocity - computed_velocity) / computed_velocity) > TRAPEZOID_ITERATION_ERROR_PERCENT);


		// set velocity and clean up any parts that are too short
		bf->cruise_velocity = computed_velocity;
		bf->head_length = mp_get_target_length(bf->entry_velocity, bf->cruise_velocity, bf);
		bf->tail_length = bf->length - bf->head_length;
		if (bf->head_length < MIN_HEAD_LENGTH) {
			bf->tail_length = bf->length;			// adjust the move to be all tail...
			bf->head_length = 0;
		}
		if (bf->tail_length < MIN_TAIL_LENGTH) {
			bf->head_length = bf->length;			//...or all head
			bf->tail_length = 0;
		}
		return;
	}

	// Requested-fit cases: remaining of: HBT, HB, BT, BT, H, T, B, cases
	bf->body_length = bf->length - bf->head_length - bf->tail_length;

	// If a non-zero body is < minimum length distribute it to the head and/or tail
	// This will generate small (acceptable) velocity errors in runtime execution
	// but preserve correct distance, which is more important.
	if ((bf->body_length < MIN_BODY_LENGTH) && (fp_NOT_ZERO(bf->body_length))) {
		if (fp_NOT_ZERO(bf->head_length)) {
			if (fp_NOT_ZERO(bf->tail_length)) {			// HBT reduces to HT
				bf->head_length += bf->body_length/2;
				bf->tail_length += bf->body_length/2;
			} else {									// HB reduces to H
				bf->head_length += bf->body_length;
			}
		} else {										// BT reduces to T
			bf->tail_length += bf->body_length;
		}
		bf->body_length = 0;

	// If the body is a standalone make the cruise velocity match the entry velocity
	// This removes a potential velocity discontinuity at the expense of top speed
	} else if ((fp_ZERO(bf->head_length)) && (fp_ZERO(bf->tail_length))) {
		bf->cruise_velocity = bf->entry_velocity;
	}
}

/*
 * mp_get_target_length()	  - derive accel/decel length from delta V and jerk
 * mp_get_target_velocity() - derive velocity achievable from delta V and length
 *
 *	This set of functions returns the fourth thing knowing the other three.
 *
 * 	  Jm = the given maximum jerk
 *	  T  = time of the entire move
 *    Vi = initial velocity
 *    Vf = final velocity
 *	  T  = 2*sqrt((Vt-Vi)/Jm)
 *	  As = The acceleration at inflection point between convex and concave portions of the S-curve.
 *	  As = (Jm*T)/2
 *    Ar = ramp acceleration
 *	  Ar = As/2 = (Jm*T)/4
 *
 *	mp_get_target_length() is a convenient function for determining the optimal_length (L)
 *	of a line given the initial velocity (Vi), final velocity (Vf) and maximum jerk (Jm).
 *
 *	The length (distance) equation is derived from:
 *
 *	 a)	L = (Vf-Vi) * T - (Ar*T^2)/2	... which becomes b) with substitutions for Ar and T
 *	 b) L = (Vf-Vi) * 2*sqrt((Vf-Vi)/Jm) - (2*sqrt((Vf-Vi)/Jm) * (Vf-Vi))/2
 *	 c)	L = (Vf-Vi)^(3/2) / sqrt(Jm)	...is an alternate form of b) (see Wolfram Alpha)
 *	 c')L = (Vf-Vi) * sqrt((Vf-Vi)/Jm) ... second alternate form; requires Vf >= Vi
 *
 *	 Notes: Ar = (Jm*T)/4					Ar is ramp acceleration
 *			T  = 2*sqrt((Vf-Vi)/Jm)			T is time
 *			Assumes Vi, Vf and L are positive or zero
 *			Cannot assume Vf>=Vi due to rounding errors and use of PLANNER_VELOCITY_TOLERANCE
 *			  necessitating the introduction of fabs()
 *
 * 	mp_get_target_velocity() is a convenient function for determining Vf target velocity for
 *	a given the initial velocity (Vi), length (L), and maximum jerk (Jm).
 *	Equation d) is b) solved for Vf. Equation e) is c) solved for Vf. Use e) (obviously)
 *
 *	 d)	Vf = (sqrt(L)*(L/sqrt(1/Jm))^(1/6)+(1/Jm)^(1/4)*Vi)/(1/Jm)^(1/4)
 *	 e)	Vf = L^(2/3) * Jm^(1/3) + Vi
 *
 *  FYI: Here's an expression that returns the jerk for a given deltaV and L:
 * 	return(cube(deltaV / (pow(L, 0.66666666))));
 */

float mp_get_target_length(const float Vi, const float Vf, const mpBuf_t *bf)
{
//	return (Vi + Vf) * sqrt(fabs(Vf - Vi) * bf->recip_jerk);		// new formula
//	return (fabs(Vi-Vf) * sqrt(fabs(Vi-Vf) * bf->recip_jerk));		// old formula

	const float delta_v = our_abs(Vi-Vf);
		return (delta_v * sqrt(delta_v * bf->recip_jerk));		// old formula
}

/* Regarding mp_get_target_velocity:
 *
 * We do some Newton-Raphson iterations to narrow it down.
 * We need a formula that includes known variables except the one we want to find,
 * and has a root [Z(x) = 0] at the value (x) we are looking for.
 *
 *      Z(x) = zero at x -- we calculate the value from the knowns and the estimate
 *             (see below) and then subtract the known value to get zero (root) if
 *             x is the correct value.
 *      Vi   = initial velocity (known)
 *      Vf   = estimated final velocity
 *      J    = jerk (known)
 *      L    = length (know)
 *
 * There are (at least) two such functions we can use:
 *      L from J, Vi, and Vf
 *      L = sqrt((Vf - Vi) / J) (Vi + Vf)
 *   Replacing Vf with x, and subtracting the known L:
 *      0 = sqrt((x - Vi) / J) (Vi + x) - L
 *      Z(x) = sqrt((x - Vi) / J) (Vi + x) - L
 *
 *  OR
 *
 *      J from L, Vi, and Vf
 *      J = ((Vf - Vi) (Vi + Vf)�) / L�
 *  Replacing Vf with x, and subtracting the known J:
 *      0 = ((x - Vi) (Vi + x)�) / L� - J
 *      Z(x) = ((x - Vi) (Vi + x)�) / L� - J
 *
 *  L doesn't resolve to the value very quickly (it graphs near-vertical).
 *  So, we'll use J, which resolves in < 10 iterations, often in only two or three
 *  with a good estimate.
 *
 *  In order to do a Newton-Raphson iteration, we need the derivative. Here they are
 *  for both the (unused) L and the (used) J formulas above:
 *
 *  J > 0, Vi > 0, Vf > 0
 *  SqrtDeltaJ = sqrt((x-Vi) * J)
 *  SqrtDeltaOverJ = sqrt((x-Vi) / J)
 *  L'(x) = SqrtDeltaOverJ + (Vi + x) / (2*J) + (Vi + x) / (2*SqrtDeltaJ)
 *
 *  J'(x) = (2*Vi*x - Vi� + 3*x�) / L�
 */

//#define __VELOCITY_DIRECT_SOLUTION
//#define __VELOCITY_ITERATION_METHOD
#define __VELOCITY_LINEAR_SNAP

#define __VELOCITY_ITERATIONS 0		// must be 0, 1, or 2

float mp_get_target_velocity(const float Vi, const float L, const mpBuf_t *bf)
{
#ifdef __VELOCITY_DIRECT_SOLUTION
	float JmL2 = bf->jerk*square(L);
	float Vi2 = square(Vi);
	float Vi3x16 = (16 * Vi * Vi2);
	float Ia = cbrt(3*sqrt(3) * sqrt(27*square(JmL2) + (2*JmL2*Vi3x16)) + 27*JmL2 + Vi3x16);
	mm.estimate = ((Ia/cbrt(2) + 4*cbrt(2)*Vi2/Ia - Vi)/3);
#endif	// __VELOCITY_DIRECT_SOLUTION

#ifdef __VELOCITY_ITERATION_METHOD
	// 0 iterations (a reasonable estimate)
	mm.estimate = pow(L, 0.66666666) * bf->cbrt_jerk + Vi;

	if (fp_ZERO(Vi)) {
	#if (GET_VELOCITY_ITERATIONS >= 1)
		// 1st iteration
		float Jd = (3 * square(mm.estimate)) / square(L);
		float Jz = cube(mm.estimate) / square(L) - bf->jerk;
		mm.estimate -= Jz/Jd;
	#endif
	#if (GET_VELOCITY_ITERATIONS >= 2)
		// 2nd iteration
		Jd = (3 * square(mm.estimate)) / square(L);
		Jz = cube(mm.estimate) / square(L) - bf->jerk;
		mm.estimate -= Jz/Jd;
	#endif

	} else {
	#if (GET_VELOCITY_ITERATIONS >= 1)
		// 1st iteration
		float L_squared = L*L;
		float Vi_squared = Vi*Vi;
		float Jz = ((mm.estimate - Vi) * (Vi + mm.estimate) * (Vi + mm.estimate)) / L_squared - bf->jerk;
		float Jd = (2*Vi*mm.estimate - Vi_squared + 3*(mm.estimate*mm.estimate)) / L_squared;
		mm.estimate = - Jz/Jd;
	#endif

	#if (GET_VELOCITY_ITERATIONS >= 2)
		// 2nd iteration
		Jz = ((mm.estimate - Vi) * (Vi + mm.estimate) * (Vi + mm.estimate)) / L_squared - bf->jerk;
		Jd = (2*Vi*mm.estimate - Vi_squared + 3*(mm.estimate*mm.estimate)) / L_squared;
		mm.estimate = - Jz/Jd;
	#endif
	}
#endif // __VELOCITY_ITERATION_METHOD
#ifdef __VELOCITY_LINEAR_SNAP

// Here we define some static constants. Not trusting the compiler to precompile them, we precompute.
// The naming is somewhat symbolic and very verbose, but still easier to understand than "i", "j", and "k".
//f is added to the beginning when the first char would be a number:
static const float sqrt_3 = 1.732050807568877;				// sqrt(3) = 1.732050807568877
static const float third = 0.333333333333333;				// 1/3 = 0.333333333333333
static const float f3_sqrt_3 = 5.196152422706631;			// 3*sqrt(3) = 5.196152422706631
static const float f4_thirds_x_cbrt_5 = 2.279967928902263;	// 4/3*5^(1/3) = 2.279967928902263
static const float f1_15th_x_2_3_rt_5 = 0.194934515880858;	// 1/15*5^(2/3) = 0.194934515880858
//static const float SQRT_FIVE_SIXTHS = 0.9128709291753;	// sqrt(5/6)
//static const float f27_sqrt_3 = 46.765371804359679;		// 27*sqrt(3) = 46.765371804359679
//static const float f80_sqrt_3 = 138.56406460551016;		// 80*sqrt(3) = 138.56406460551016

	// Why const? So that the compiler knows it'll never change once it's computed.
	// Also, we ensure that it doesn't accidentally change once computed.
	// Average cost: 0.14ms -- 140us (ATSAM8X3C/E @ 84 MHz)
	const float j = bf->jerk;
	const float j_sq = j*j;					//j^2

	const float v_0_sq = Vi*Vi;				//v_0^2
	const float v_0_cu = v_0_sq*Vi;			//v_0^3
	const float v_0_cu_x_40 = v_0_cu*40;	//v_0^3*40

	const float L_sq = L*L;								//L^2
	const float L_sq_x_j_x_sqrt_3 = L_sq * j * sqrt_3;	//L^2*j*sqrt(3)
	const float L_fourth = L_sq*L_sq;					//L^4

	// v_1 = 4/3*5^(1/3) *  v_0^2 /(27*sqrt(3)*L^2*j + 40*v_0^3 + 3*sqrt(3)*sqrt(80*sqrt(3)*L^2*j*v_0^3 + 81*L^4*j^2))^(1/3)
	//        + 1/15*5^(2/3)*(27*sqrt(3)*L^2*j + 40*v_0^3 + 3*sqrt(3)*sqrt(80*sqrt(3)*L^2*j*v_0^3 + 81*L^4*j^2))^(1/3)
	//        - 1/3*v_0

	// chunk_1 = pow( (27 * sqrt(3)*L^2*j     + 40 * v_0^3  + 3*sqrt(3) * sqrt(80 * v_0^3  * sqrt(3)*L^2*j     + 81 * L^4      * j^2 ) ), third)
	//         = pow( (27 * L_sq_x_j_x_sqrt_3 + 40 * v_0_cu + f3_sqrt_3 * sqrt(80 * v_0_cu * L_sq_x_j_x_sqrt_3 + 81 * L_fourth * j_sq) ), third)

	//         = pow( (27 * L_sq_x_j_x_sqrt_3 + 40 * v_0_cu + f3_sqrt_3 * sqrt(2 * 40 * v_0_cu * L_sq_x_j_x_sqrt_3 + 81 * L_fourth * j_sq) ), third)
	//         = pow( (27 * L_sq_x_j_x_sqrt_3 + v_0_cu_x_40 + f3_sqrt_3 * sqrt(2 * v_0_cu_x_40 * L_sq_x_j_x_sqrt_3 + 81 * L_fourth * j_sq) ), third)

	const float chunk_1_cubed = (27 * L_sq_x_j_x_sqrt_3 + v_0_cu_x_40 + f3_sqrt_3 * sqrt(2 * v_0_cu_x_40 * L_sq_x_j_x_sqrt_3 + 81 * L_fourth * j_sq) );
	const float chunk_1 = cbrtf(chunk_1_cubed);

	// v_1 = 4/3*5^(1/3)        * v_0^2  / chunk_1  +   1/15*5^(2/3)       * chunk_1  -  1/3  *v_0
	// v_1 = f4_thirds_x_cbrt_5 * v_0_sq / chunk_1  +   f1_15th_x_2_3_rt_5 * chunk_1  -  third*v_0

	float v_1 = (f4_thirds_x_cbrt_5 * v_0_sq) / chunk_1  +  f1_15th_x_2_3_rt_5 * chunk_1  -  third * Vi;
//	return v_1;
	mm.estimate = v_1;
#endif	// __VELOCITY_LINEAR_SNAP

	return mm.estimate;
}

/*
 * mp_get_meet_velocity()
 */
const float mv_constant = 0.60070285354;				// sqrt(5) / (2 sqrt(2) nroot(3,4)) = 0.60070285354

// Just calling this tl_constant. It's full name is:
static const float tl_constant = 1.201405707067378;		// sqrt(5)/( sqrt(2)pow(3,4) )

float mp_get_meet_velocity(const float v_0, const float v_2, const float L, const mpBuf_t *bf) {
	//L_d(v_0, v_1, j) = (sqrt(5) abs(v_0 - v_1) (v_0 - 3v_1)) / (2sqrt(2) 3^(1 / 4) sqrt(j abs(v_0 - v_1)) (v_0 - v_1))
	//                    sqrt(5) / (2 sqrt(2) nroot(3,4)) ( v_0 - 3 v_1) / ( sqrt(j abs(v_0 - v_1)))
	//                    sqrt(5) / (2 sqrt(2) nroot(3,4)) = 0.60070285354

	// Average cost: 0.18ms -- 180us

	const float j = bf->jerk;
	const float recip_j = bf->recip_jerk;

	float l_c_head, l_c_tail, l_c; // calculated L
	float l_d_head, l_d_tail, l_d; // calculated derivative of L with respect to v_1

	// chunks of precomputed values
	float sqrt_j_delta_v_0, sqrt_j_delta_v_1;

	// v_1 is our estimated return value.
	// We estimate with the speed obtained by L/2 traveled from the highest speed of v_0 or v_2.
	float v_1 = mp_get_target_velocity(max(v_0, v_2), L/2, bf);
	float last_v_1 = 0;

	// Per iteration: 2 sqrt, 2 abs, 6 -, 4 +, 12 *, 3 /
	int i = 0; // limit the iterations
	while (i++ < TRAPEZOID_ITERATION_MAX && our_abs(last_v_1 - v_1) < 2) {
		last_v_1 = v_1;

		// Precompute some common chunks
		sqrt_j_delta_v_0 = sqrt(j * our_abs(v_1-v_0));
		sqrt_j_delta_v_1 = sqrt(j * our_abs(v_1-v_2));

		l_c_head = tl_constant * sqrt_j_delta_v_0 * (v_0+v_1) * recip_j;
		l_c_tail = tl_constant * sqrt_j_delta_v_1 * (v_2+v_1) * recip_j;

		// l_c is our total-length calculation wih the curent v_1 estimate, minus the expected length.
		// This makes l_c == 0 when v_1 is the correct value.
		l_c = (l_c_head + l_c_tail) - L;

		// Early escape -- if we're within 2 of "root" then we can call it good.
		if (our_abs(l_c) < 2) {
			break;
		}

		// l_d is the derivative of l_c, and is used for the Newton-Raphson iteration.
		l_d_head = (mv_constant * (v_0 - 3*v_1)) / sqrt_j_delta_v_0;
		l_d_tail = (mv_constant * (v_2 - 3*v_1)) / sqrt_j_delta_v_1;
		l_d = l_d_head + l_d_tail;

		v_1 = v_1 - (l_c/l_d);
	}

	return v_1;
}
/*
#ifdef __cplusplus
}
#endif
*/