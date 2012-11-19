/*
        Copyright (C) 2010 Stephen M. Cameron
        Author: Stephen M. Cameron

        This file is part of Spacenerds In Space.

        Spacenerds in Space is free software; you can redistribute it and/or modify
        it under the terms of the GNU General Public License as published by
        the Free Software Foundation; either version 2 of the License, or
        (at your option) any later version.

        Spacenerds in Space is distributed in the hope that it will be useful,
        but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
        GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
        along with Spacenerds in Space; if not, write to the Free Software
        Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <linux/tcp.h>
#include <stddef.h>

#include "ssgl/ssgl.h"
#include "snis.h"
#include "mathutils.h"
#include "snis_alloc.h"
#include "snis_marshal.h"
#include "snis_socket_io.h"
#include "snis_packet.h"
#include "sounds.h"
#include "names.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#define CLIENT_UPDATE_PERIOD_NSECS 500000000
#define MAXCLIENTS 100
struct game_client {
	int socket;
	pthread_t read_thread;
	pthread_t write_thread;
	pthread_attr_t read_attr, write_attr;

	struct packed_buffer_queue client_write_queue;
	pthread_mutex_t client_write_queue_mutex;
	uint32_t shipid;
	uint8_t role;
	uint32_t timestamp;
} client[MAXCLIENTS];
int nclients = 0;
static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

struct bridge_credentials {
	unsigned char shipname[20];
	unsigned char password[20];
	uint32_t shipid;
} bridgelist[MAXCLIENTS];
int nbridges = 0;
static pthread_mutex_t universe_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t listener_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t listener_started;
int listener_port = -1;
pthread_t lobbythread;

static inline void client_lock()
{
        (void) pthread_mutex_lock(&client_mutex);
}

static inline void client_unlock()
{
        (void) pthread_mutex_unlock(&client_mutex);
}


int nframes = 0;
int timer = 0;
struct timeval start_time, end_time;

static struct snis_object_pool *pool;
static struct snis_entity go[MAXGAMEOBJS];
static uint32_t universe_timestamp = 1;

static uint32_t get_new_object_id(void)
{
	static uint32_t current_id = 0;
	static uint32_t answer;
	static pthread_mutex_t object_id_lock = PTHREAD_MUTEX_INITIALIZER;

	pthread_mutex_lock(&object_id_lock);
	answer = current_id++;
	pthread_mutex_unlock(&object_id_lock);	
	return answer;
}

static void generic_move(__attribute__((unused)) struct snis_entity *o)
{
	return;
}

static void queue_delete_oid(struct game_client *c, uint32_t oid)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct delete_object_packet));
	packed_buffer_append_u16(pb, OPCODE_DELETE_OBJECT);
	packed_buffer_append_u32(pb, oid);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void snis_queue_delete_object(uint32_t oid)
{
	/* Iterate over all clients and inform them that
	 * an object is gone.
	 * Careful with the locking, we have 3 layers of locks
	 * here (couldn't figure out a simpler way.
	 * 
	 * We should already hold the universe_mutex, we 
	 * need the client lock to iterate over the clients,
	 * and when we build the packet, we'll need the
	 * client_write_queue_mutexes.  Hope there's no deadlocks.
	 */
	int i;

	client_lock();
	for (i = 0; i < nclients; i++)
		queue_delete_oid(&client[i], oid);
	client_unlock();
}

static void send_packet_to_all_clients_on_a_bridge(uint32_t shipid, struct packed_buffer *pb)
{
	int i;

	client_lock();
	for (i = 0; i < nclients; i++) {
		struct packed_buffer *pbc;
		struct game_client *c = &client[i];

		if (c->shipid != shipid)
			continue;

		pbc = packed_buffer_copy(pb);
		packed_buffer_queue_add(&c->client_write_queue, pbc, &c->client_write_queue_mutex);
	}
	packed_buffer_free(pb);
	client_unlock();
}

static void queue_add_sound(struct game_client *c, uint16_t sound_number)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct play_sound_packet));
	packed_buffer_append_u16(pb, OPCODE_PLAY_SOUND);
	packed_buffer_append_u16(pb, sound_number);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void snis_queue_add_sound(uint16_t sound_number, __attribute__((unused)) uint32_t roles)
{
	int i;
	client_lock();
	for (i = 0; i < nclients; i++)
		/* later, change code to check if client has the right role first... */
		queue_add_sound(&client[i], sound_number);
	client_unlock();
}

static int add_explosion(double x, double y, uint16_t velocity, uint16_t nsparks, uint16_t time);

static void normalize_coords(struct snis_entity *o)
{
	if (o->x < 0)
		o->x += XUNIVERSE_DIMENSION;
	else
		if (o->x > XUNIVERSE_DIMENSION)
			o->x -= XUNIVERSE_DIMENSION;
	if (o->y < 0)
		o->y += YUNIVERSE_DIMENSION;
	else
		if (o->y > YUNIVERSE_DIMENSION)
			o->y -= YUNIVERSE_DIMENSION;
}

static void torpedo_move(struct snis_entity *o)
{
	int i;

	o->x += o->vx;
	o->y += o->vy;
	normalize_coords(o);
	o->timestamp = universe_timestamp;
	o->alive--;
	for (i = 0; i <= snis_object_pool_highest_object(pool); i++) {
		double dist2;

		if (!go[i].alive)
			continue;
		if (i == o->index)
			continue;
		if (o->alive >= TORPEDO_LIFETIME - 3)
			continue;
		if (go[i].type != OBJTYPE_SHIP1 && go[i].type != OBJTYPE_SHIP2)
			continue;
		if (go[i].id == o->tsd.torpedo.ship_id)
			continue; /* can't torpedo yourself. */
		dist2 = ((go[i].x - o->x) * (go[i].x - o->x)) +
			((go[i].y - o->y) * (go[i].y - o->y));

		if (dist2 > TORPEDO_DETONATE_DIST2)
			continue; /* not close enough */

		/* hit!!!! */
		o->alive = 0;
		go[i].alive = 0;
		(void) add_explosion(go[i].x, go[i].y, 50, 50, 50);
		snis_queue_delete_object(go[i].id);
		snis_queue_add_sound(EXPLOSION_SOUND, 0xffff);
		snis_object_pool_free_object(pool, i);
		continue;
	}

	if (o->alive <= 0) {
		snis_queue_delete_object(o->id);
		o->alive = 0;
		snis_object_pool_free_object(pool, o->index);
	}
}

static void ship_move(struct snis_entity *o)
{
	double heading_diff, yaw_vel;

	/* Decide where to go... */
	if (snis_randn(100) < 5) {
		o->tsd.ship.desired_heading = degrees_to_radians(0.0 + snis_randn(360)); 
		o->tsd.ship.desired_velocity = snis_randn(20);
	}

	/* Adjust heading towards desired heading */
	heading_diff = o->tsd.ship.desired_heading - o->heading;
	if (heading_diff < -M_PI)
		heading_diff += 2.0 * M_PI;
	if (heading_diff > M_PI)
		heading_diff -= 2.0 * M_PI;
	if (heading_diff > MAX_YAW_VELOCITY) {
		yaw_vel = MAX_YAW_VELOCITY;
	} else {
		if (heading_diff < -MAX_YAW_VELOCITY) {
			yaw_vel = -MAX_YAW_VELOCITY;
		} else {
			yaw_vel = heading_diff * 0.8;
		}
	}
	if (fabs(heading_diff) < (M_PI / 180.0))
		yaw_vel = heading_diff;
	o->heading += yaw_vel;
	normalize_angle(&o->heading);

	/* Adjust velocity towards desired velocity */
	o->tsd.ship.velocity = o->tsd.ship.velocity +
			(o->tsd.ship.desired_velocity - o->tsd.ship.velocity) * 0.3;
	if (fabs(o->tsd.ship.velocity - o->tsd.ship.desired_velocity) < 2.0)
		o->tsd.ship.velocity = o->tsd.ship.desired_velocity;

	/* set vx, vy, move x, y */
	o->vx = o->tsd.ship.velocity * sin(o->heading);
	o->vy = o->tsd.ship.velocity * -cos(o->heading);
	o->x += o->vx;
	o->y += o->vy;
	normalize_coords(o);
	o->timestamp = universe_timestamp;
}

static void damp_yaw_velocity(double *yv, double damp_factor)
{
	if (fabs(*yv) < (0.3 * PI / 180.0))
		*yv = 0.0;
	else
		*yv *= damp_factor;
}

static double rpmx[] = {
	 -1.0, 0.0,
		255.0 / 5.0,
		2.0 * 255.0 / 5.0,
		3.0 * 255.0 / 5.0,
		4.0 * 255.0 / 5.0,
		256.0 };

static double powery[] =  {
	0.0, 0.0, 
		0.33 * (double) UINT32_MAX, 
		0.6 * (double) UINT32_MAX, 
		0.8 * (double) UINT32_MAX,  
		0.95 * (double) UINT32_MAX,  
		0.8 * (double) UINT32_MAX,  } ;

static double tempy[] = {
	0.0, 0.0,
		0.3 * (double) UINT8_MAX,
		0.5 * (double) UINT8_MAX,
		0.55 * (double) UINT8_MAX,
		0.65 * (double) UINT8_MAX,
		1.0 * (double) UINT8_MAX,
	};

static double powertempy[] = {
	0.0, 0.0, 
		0.4,
		0.5,
		0.7,
		0.8,
		1.0,
};
		

static void player_move(struct snis_entity *o)
{
	int desired_rpm, desired_temp, diff;

	o->vy = o->tsd.ship.velocity * cos(o->heading);
	o->vx = o->tsd.ship.velocity * -sin(o->heading);
	o->x += o->vx;
	o->y += o->vy;
	normalize_coords(o);
	o->heading += o->tsd.ship.yaw_velocity;
	o->tsd.ship.gun_heading += o->tsd.ship.gun_yaw_velocity;
	o->tsd.ship.sci_heading += o->tsd.ship.sci_yaw_velocity;
	o->tsd.ship.shields = universe_timestamp % 100;

	normalize_angle(&o->heading);
	normalize_angle(&o->tsd.ship.gun_heading);
	normalize_angle(&o->tsd.ship.sci_heading);
	o->timestamp = universe_timestamp;

	damp_yaw_velocity(&o->tsd.ship.yaw_velocity, YAW_DAMPING);
	damp_yaw_velocity(&o->tsd.ship.gun_yaw_velocity, GUN_YAW_DAMPING);
	damp_yaw_velocity(&o->tsd.ship.sci_yaw_velocity, SCI_YAW_DAMPING);

	/* Damp velocity */
	if (fabs(o->tsd.ship.velocity) < MIN_PLAYER_VELOCITY)
		o->tsd.ship.velocity = 0.0;
	else
		o->tsd.ship.velocity *= PLAYER_VELOCITY_DAMPING;

	/* check to see if any torpedoes have finished loading */
	if (o->tsd.ship.torpedo_load_time > 0) {
		o->tsd.ship.torpedo_load_time--;
		if (o->tsd.ship.torpedo_load_time == 0) {
			o->tsd.ship.torpedoes_loaded++;
			o->tsd.ship.torpedoes_loading = 0;
		}
	}
	/* calculate fuel/power/rpm, etc. */
	desired_rpm = o->tsd.ship.throttle;
	if (o->tsd.ship.rpm < desired_rpm) {
		diff = desired_rpm - o->tsd.ship.rpm;
		if (diff > 10) {
			diff = 10;
		} else {
			diff = diff / 2;
			if (diff <= 0)
				diff = 1;
		}
		o->tsd.ship.rpm += diff;
	} else {
		if (o->tsd.ship.rpm > desired_rpm) {
			diff = o->tsd.ship.rpm - desired_rpm;
			if (diff > 10) {
				diff = 10;
			} else {
				diff = diff / 2;
				if (diff <= 0)
					diff = 1;
			}
			o->tsd.ship.rpm -= diff;
		}
	}
	o->tsd.ship.fuel -= (int) (o->tsd.ship.rpm * FUEL_USE_FACTOR);
	o->tsd.ship.power = table_interp((double) o->tsd.ship.rpm,
			rpmx, powery, ARRAY_SIZE(rpmx));
	desired_temp = (uint8_t) table_interp((double) o->tsd.ship.rpm,
			rpmx, tempy, ARRAY_SIZE(rpmx));
	desired_rpm = o->tsd.ship.throttle;
	if (snis_randn(100) < 50) { /* adjust temp slowly, stochastically */
		diff = 0;
		if (o->tsd.ship.temp < desired_temp) {
			diff = 1;
		} else {
			if (o->tsd.ship.temp > desired_temp) {
				diff = -1;
			}
		}
		o->tsd.ship.temp += diff;
	}
	o->tsd.ship.power *= table_interp((double) o->tsd.ship.temp,
			rpmx, powertempy, ARRAY_SIZE(powertempy));
}

static void starbase_move(struct snis_entity *o)
{
	/* FIXME, fill this in. */
}

static void explosion_move(struct snis_entity *o)
{
	if (o->alive > 0)
		o->alive--;

	if (o->alive <= 0) {
		snis_queue_delete_object(o->id);
		o->alive = 0;
		snis_object_pool_free_object(pool, o->index);
	}
}

static int add_generic_object(double x, double y, double vx, double vy, double heading, int type)
{
	int i;
	char *n;

	i = snis_object_pool_alloc_obj(pool); 	 
	if (i < 0)
		return -1;
	n = random_name();
	memset(&go[i], 0, sizeof(go[i]));
	go[i].id = get_new_object_id();
	go[i].index = i;
	go[i].alive = 1;
	go[i].x = x;
	go[i].y = y;
	go[i].vx = vx;
	go[i].vy = vy;
	go[i].heading = heading;
	go[i].type = type;
	go[i].timestamp = universe_timestamp + 1;
	go[i].move = generic_move;
	strncpy(go[i].sdata.name, n, sizeof(go[i].sdata.name) - 1);
	free(n);
	return i;
}

static int add_player(double x, double y, double vx, double vy, double heading)
{
	int i;

	i = add_generic_object(x, y, vx, vy, heading, OBJTYPE_SHIP1);
	if (i < 0)
		return i;
	go[i].move = player_move;
	go[i].tsd.ship.torpedoes = 1000;
	go[i].tsd.ship.shields = 100.0;
	go[i].tsd.ship.power = 100.0;
	go[i].tsd.ship.yaw_velocity = 0.0;
	go[i].tsd.ship.gun_yaw_velocity = 0.0;
	go[i].tsd.ship.gun_heading = 0.0;
	go[i].tsd.ship.velocity = 0.0;
	go[i].tsd.ship.desired_velocity = 0.0;
	go[i].tsd.ship.desired_heading = 0.0;
	go[i].tsd.ship.sci_beam_width = MAX_SCI_BW_YAW_VELOCITY;
	go[i].tsd.ship.fuel = UINT32_MAX;
	go[i].tsd.ship.rpm = 0;
	go[i].tsd.ship.temp = 0;
	go[i].tsd.ship.power = 0;
	return i;
}

static int add_ship(double x, double y, double vx, double vy, double heading)
{
	int i;

	i = add_generic_object(x, y, vx, vy, heading, OBJTYPE_SHIP2);
	if (i < 0)
		return i;
	go[i].move = ship_move;
	go[i].tsd.ship.torpedoes = 0;
	go[i].tsd.ship.shields = 100.0;
	go[i].tsd.ship.power = 100.0;
	go[i].tsd.ship.yaw_velocity = 0.0;
	go[i].tsd.ship.desired_velocity = 0;
	go[i].tsd.ship.desired_heading = 0;
	go[i].tsd.ship.velocity = 0;
	go[i].tsd.ship.shiptype = snis_randn(ARRAY_SIZE(shipclass));
	return i;
}

static int add_planet(double x, double y, double vx, double vy, double heading)
{
	return add_generic_object(x, y, vx, vy, heading, OBJTYPE_PLANET);
}

static int add_starbase(double x, double y, double vx, double vy, double heading)
{
	int i;

	i = add_generic_object(x, y, vx, vy, heading, OBJTYPE_STARBASE);
	if (i < 0)
		return i;
	go[i].move = starbase_move;
	go[i].type = OBJTYPE_STARBASE;
	return i;
}

static int add_explosion(double x, double y, uint16_t velocity, uint16_t nsparks, uint16_t time)
{
	int i;

	i = add_generic_object(x, y, 0, 0, 0, OBJTYPE_EXPLOSION);
	if (i < 0)
		return i;
	go[i].move = explosion_move;
	go[i].alive = 30; /* long enough to get propagaed out to all clients */
	go[i].tsd.explosion.velocity = velocity;
	go[i].tsd.explosion.nsparks = nsparks;
	go[i].tsd.explosion.time = time;
	return i;
}

static int __attribute__((unused)) add_laser(double x, double y, double vx, double vy, double heading)
{
	return add_generic_object(x, y, vx, vy, heading, OBJTYPE_LASER);
}

static int add_torpedo(double x, double y, double vx, double vy, double heading, uint32_t ship_id)
{
	int i;
	i = add_generic_object(x, y, vx, vy, heading, OBJTYPE_TORPEDO);
	if (i < 0)
		return i;
	go[i].move = torpedo_move;
	go[i].alive = TORPEDO_LIFETIME;
	go[i].tsd.torpedo.ship_id = ship_id;
	return i;
}

static void __attribute__((unused)) add_starbases(void)
{
	int i;
	double x, y;

	for (i = 0; i < NBASES; i++) {
		x = ((double) snis_randn(1000)) * XUNIVERSE_DIMENSION / 1000.0;
		y = ((double) snis_randn(1000)) * YUNIVERSE_DIMENSION / 1000.0;
		add_starbase(x, y, 0.0, 0.0, 0.0);
	}
}

static void add_planets(void)
{
	int i;
	double x, y;

	for (i = 0; i < NPLANETS; i++) {
		x = ((double) snis_randn(1000)) * XUNIVERSE_DIMENSION / 1000.0;
		y = ((double) snis_randn(1000)) * YUNIVERSE_DIMENSION / 1000.0;
		add_planet(x, y, 0.0, 0.0, 0.0);
	}
}

static void add_eships(void)
{
	int i;
	double x, y, heading;

	for (i = 0; i < NESHIPS; i++) {
		x = ((double) snis_randn(1000)) * XUNIVERSE_DIMENSION / 1000.0;
		y = ((double) snis_randn(1000)) * YUNIVERSE_DIMENSION / 1000.0;
		heading = degrees_to_radians(0.0 + snis_randn(360)); 
		add_ship(x, y, 0.0, 0.0, heading);
	}
}

static void make_universe(void)
{
	pthread_mutex_lock(&universe_mutex);
	snis_object_pool_setup(&pool, MAXGAMEOBJS);

	add_starbases();
	add_planets();
	add_eships();
	pthread_mutex_unlock(&universe_mutex);
}

static void __attribute__((unused)) timespec_subtract(struct timespec *have, struct timespec *takeaway, struct timespec *leaves)
{
	leaves->tv_nsec = have->tv_nsec - takeaway->tv_nsec;
	if (have->tv_nsec < takeaway->tv_nsec) {
		leaves->tv_nsec += 1000000000;
		leaves->tv_sec = have->tv_sec - takeaway->tv_sec - 1;
	} else {
		leaves->tv_sec = have->tv_sec - takeaway->tv_sec;
	}
}

static void sleep_tenth_second(void)
{
	struct timespec t, x;
	int rc;

	t.tv_sec = 0;
	t.tv_nsec = 99999999; 
	x.tv_sec = 0;
	x.tv_nsec = 0;

	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, 0,
				(const struct timespec *) &t, &x);
	} while (rc == EINTR);
}

static void sleep_thirtieth_second(void)
{
	struct timespec t, x;
	int rc;

	t.tv_sec = 0;
	t.tv_nsec = 99999999; 
	x.tv_sec = 0;
	x.tv_nsec = 0;

	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &t, &x);
	} while (rc == EINTR);
}

#if 0
/* Sleep for enough time, x, such that (end - begin + x) == total*/
static void snis_sleep(struct timespec *begin, struct timespec *end, struct timespec *total)
{
#if 0

	/* this code seems to be buggy. */
	int rc;
	struct timespec used, diff, remain;

	timespec_subtract(end, begin, &used);
	if (used.tv_nsec >= total->tv_nsec)
		return; 

	timespec_subtract(total, &used, &remain);
	do {
		diff = remain;
		rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &diff, &remain);	
	} while (rc == EINTR);
#else
	int rc;

	do {
		rc = clock_nanosleep(CLOCK_MONOTONIC, 0,
			(const struct timespec *) total, begin);
	} while (rc == EINTR);
#endif
}
#endif

static void do_thrust(struct game_client *c, int thrust)
{
	struct snis_entity *ship = &go[c->shipid];

	if (thrust > 0) {
		if (ship->tsd.ship.velocity < MAX_PLAYER_VELOCITY)
			ship->tsd.ship.velocity += PLAYER_VELOCITY_INCREMENT;
	} else {
		if (ship->tsd.ship.velocity > -MAX_PLAYER_VELOCITY)
			ship->tsd.ship.velocity -= PLAYER_VELOCITY_INCREMENT;
	}
}

typedef void (*do_yaw_function)(struct game_client *c, int yaw);

static void do_generic_yaw(double *yawvel, int yaw, double max_yaw, double yaw_inc)
{
	if (yaw > 0) {
		if (*yawvel < max_yaw)
			*yawvel += yaw_inc;
	} else {
		if (*yawvel > -max_yaw)
			*yawvel -= yaw_inc;
	}
}

static void do_yaw(struct game_client *c, int yaw)
{
	struct snis_entity *ship = &go[c->shipid];

	do_generic_yaw(&ship->tsd.ship.yaw_velocity, yaw, MAX_YAW_VELOCITY, YAW_INCREMENT);
}

static void do_gun_yaw(struct game_client *c, int yaw)
{
	/* FIXME combine this with do_yaw somehow */
	struct snis_entity *ship = &go[c->shipid];

	do_generic_yaw(&ship->tsd.ship.gun_yaw_velocity, yaw,
				MAX_GUN_YAW_VELOCITY, GUN_YAW_INCREMENT);
}

static void do_sci_yaw(struct game_client *c, int yaw)
{
	struct snis_entity *ship = &go[c->shipid];

	do_generic_yaw(&ship->tsd.ship.sci_yaw_velocity, yaw,
				MAX_SCI_YAW_VELOCITY, SCI_YAW_INCREMENT);
}

static void do_sci_bw_yaw(struct game_client *c, int yaw)
{
	struct snis_entity *ship = &go[c->shipid];

	do_generic_yaw(&ship->tsd.ship.sci_beam_width, yaw,
			MAX_SCI_BW_YAW_VELOCITY, SCI_BW_YAW_INCREMENT);
	ship->tsd.ship.sci_beam_width = fabs(ship->tsd.ship.sci_beam_width);
	if (ship->tsd.ship.sci_beam_width < MIN_SCI_BEAM_WIDTH)
		ship->tsd.ship.sci_beam_width = MIN_SCI_BEAM_WIDTH;
}

static int process_request_thrust(struct game_client *c)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	uint8_t thrust;
	int rc;

	rc = snis_readsocket(c->socket, buffer, sizeof(struct request_thrust_packet) - sizeof(uint16_t));
	if (rc)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	thrust = packed_buffer_extract_u8(&pb);
	switch (thrust) {
	case THRUST_FORWARDS:
		do_thrust(c, 1);
		break;
	case THRUST_BACKWARDS:
		do_thrust(c, -1);
		break;
	default:
		break;
	}
	return 0;
}

/* must hold universe mutex */
static int lookup_by_id(uint32_t id)
{
	int i;

	for (i = 0; i <= snis_object_pool_highest_object(pool); i++)
		if (go[i].id == id)
			return i;
	return -1;
}

static void send_ship_sdata_packet(struct game_client *c, struct ship_sdata_packet *sip);
static int process_request_ship_sdata(struct game_client *c)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	uint16_t id;
	int i;
	int rc;
	struct ship_sdata_packet p; 

	rc = snis_readsocket(c->socket, buffer, sizeof(struct request_ship_sdata_packet) - sizeof(uint16_t));
	if (rc)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	memset(p.name, 0, sizeof(p.name));
	pthread_mutex_lock(&universe_mutex);
	i = lookup_by_id(id);
	if (i < 0) {
		pthread_mutex_unlock(&universe_mutex);
		return -1;
	}
	strcpy(p.name, go[i].sdata.name);
	p.id = id;
	if (go[i].type == OBJTYPE_SHIP2)
		p.subclass = go[i].tsd.ship.shiptype;
	else
		p.subclass = 0;
	pthread_mutex_unlock(&universe_mutex);
	send_ship_sdata_packet(c, &p);
	return 0;
}

static int process_request_bytevalue_pwr(struct game_client *c, int offset)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	int i, rc;
	uint32_t id;
	uint8_t *bytevalue;
	uint8_t v;

	rc = snis_readsocket(c->socket, buffer, sizeof(struct request_throttle_packet) - sizeof(uint16_t));
	if (rc)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	id = packed_buffer_extract_u32(&pb);
	v = packed_buffer_extract_u8(&pb);
	pthread_mutex_lock(&universe_mutex);
	i = lookup_by_id(id);
	if (i < 0) {
		pthread_mutex_unlock(&universe_mutex);
		return -1;
	}
	if (i != c->shipid)
		printf("i != ship id\n");
	bytevalue = (uint8_t *) &go[i];
	bytevalue += offset;
	*bytevalue = v;
	pthread_mutex_unlock(&universe_mutex);
	return 0;
}


static int process_request_throttle(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.throttle)); 
}

static int process_request_maneuvering_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.maneuvering)); 
}

static int process_request_warp_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.warp)); 
}

static int process_request_impulse_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.impulse)); 
}

static int process_request_sensors_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.sensors)); 
}

static int process_request_comms_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.comms)); 
}

static int process_request_shields_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.shields)); 
}

static int process_request_phaserbanks_pwr(struct game_client *c)
{
	return process_request_bytevalue_pwr(c, offsetof(struct snis_entity, tsd.ship.pwrdist.phaserbanks)); 
}

static int process_request_yaw(struct game_client *c, do_yaw_function yaw_func)
{
	unsigned char buffer[10];
	struct packed_buffer pb;
	uint8_t yaw;
	int rc;

	rc = snis_readsocket(c->socket, buffer, sizeof(struct request_yaw_packet) - sizeof(uint16_t));
	if (rc)
		return rc;
	packed_buffer_init(&pb, buffer, sizeof(buffer));
	yaw = packed_buffer_extract_u8(&pb);
	switch (yaw) {
	case YAW_LEFT:
		yaw_func(c, -1);
		break;
	case YAW_RIGHT:
		yaw_func(c, 1);
		break;
	default:
		break;
	}
	return 0;
}

static int process_load_torpedo(struct game_client *c)
{
	struct snis_entity *ship = &go[c->shipid];
	// struct packed_buffer *pb;

	if (ship->tsd.ship.torpedoes < 0)
		return -1;
	if (ship->tsd.ship.torpedoes_loading != 0)
		return -1;
	if (ship->tsd.ship.torpedoes_loaded + ship->tsd.ship.torpedoes_loading >= 2)
		return -1;
	ship->tsd.ship.torpedoes--;
	ship->tsd.ship.torpedoes_loading++;
	ship->tsd.ship.torpedo_load_time = 4 * 10; /* 4 secs */ 
	return 0;
}

static int process_request_torpedo(struct game_client *c)
{
	struct snis_entity *ship = &go[c->shipid];
	double vx, vy;

	if (ship->tsd.ship.torpedoes_loaded <= 0)
		return 0;

	vx = TORPEDO_VELOCITY * sin(ship->tsd.ship.gun_heading);
	vy = TORPEDO_VELOCITY * -cos(ship->tsd.ship.gun_heading);
	pthread_mutex_lock(&universe_mutex);
	add_torpedo(ship->x, ship->y, vx, vy, ship->tsd.ship.gun_heading, ship->id); 
	ship->tsd.ship.torpedoes_loaded--;
	snis_queue_add_sound(TORPEDO_LAUNCH_SOUND, 0xffff);
	pthread_mutex_unlock(&universe_mutex);
	return 0;
}

static int process_request_phaser(__attribute__((unused)) struct game_client *c)
{
	return 0;
}

static void process_instructions_from_client(struct game_client *c)
{
	int rc;
	uint16_t opcode;

	rc = snis_readsocket(c->socket, &opcode, sizeof(opcode));
	if (rc != 0)
		goto protocol_error;
	opcode = ntohs(opcode);

	switch (opcode) {
		case OPCODE_REQUEST_TORPEDO:
			process_request_torpedo(c);
			break;
		case OPCODE_REQUEST_PHASER:
			process_request_phaser(c);
			break;
		case OPCODE_LOAD_TORPEDO:
			process_load_torpedo(c);
			break;
		case OPCODE_REQUEST_YAW:
			rc = process_request_yaw(c, do_yaw);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_THROTTLE:
			rc = process_request_throttle(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_MANEUVERING_PWR:
			rc = process_request_maneuvering_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_WARP_PWR:
			rc = process_request_warp_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_IMPULSE_PWR:
			rc = process_request_impulse_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_SHIELDS_PWR:
			rc = process_request_shields_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_SENSORS_PWR:
			rc = process_request_sensors_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_COMMS_PWR:
			rc = process_request_comms_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_PHASERBANKS_PWR:
			rc = process_request_phaserbanks_pwr(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_GUNYAW:
			rc = process_request_yaw(c, do_gun_yaw);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_SCIYAW:
			rc = process_request_yaw(c, do_sci_yaw);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_SCIBEAMWIDTH:
			rc = process_request_yaw(c, do_sci_bw_yaw);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_REQUEST_THRUST:
			rc = process_request_thrust(c);
			if (rc)
				goto protocol_error;
			break;
		case OPCODE_NOOP:
			break;
		case OPCODE_REQUEST_SHIP_SDATA:
			rc = process_request_ship_sdata(c);
			if (rc)
				goto protocol_error;
			break;
		default:
			goto protocol_error;
	}
	return;

protocol_error:
	printf("Protocol error in process_instructions_from_client\n");
	shutdown(c->socket, SHUT_RDWR);
	close(c->socket);
	c->socket = -1;
	return;
}

static void *per_client_read_thread(__attribute__((unused)) void /* struct game_client */ *client)
{
	struct game_client *c = (struct game_client *) client;

	/* Wait for client[] array to get fully updated before proceeding. */
	client_lock();
	client_unlock();
	while (1) {
		process_instructions_from_client(c);
		if (c->socket < 0)
			break;
	}
	printf("client reader thread exiting\n");
	return NULL;
}

static void write_queued_updates_to_client(struct game_client *c)
{
	/* write queued updates to client */
	int rc;
	uint16_t noop = 0xffff;

	struct packed_buffer *buffer;

	/*  packed_buffer_queue_print(&c->client_write_queue); */
	buffer = packed_buffer_queue_combine(&c->client_write_queue, &c->client_write_queue_mutex);
	if (buffer->buffer_size > 0) {
		rc = snis_writesocket(c->socket, buffer->buffer, buffer->buffer_size);
		if (rc != 0) {
			printf("writesocket failed, rc= %d, errno = %d(%s)\n", 
				rc, errno, strerror(errno));
			goto badclient;
		}
	} else {
		/* no-op, just so we know if client is still there */
		rc = snis_writesocket(c->socket, &noop, sizeof(noop));
		if (rc != 0) {
			printf("(noop) writesocket failed, rc= %d, errno = %d(%s)\n", 
				rc, errno, strerror(errno));
			goto badclient;
		}
	}
	packed_buffer_free(buffer);
	return;

badclient:
	shutdown(c->socket, SHUT_RDWR);
	close(c->socket);
	c->socket = -1;
}

static void send_update_ship_packet(struct game_client *c,
	struct snis_entity *o, uint16_t opcode);
static void send_econ_update_ship_packet(struct game_client *c,
	struct snis_entity *o);
static void send_update_planet_packet(struct game_client *c,
	struct snis_entity *o);
static void send_update_starbase_packet(struct game_client *c,
	struct snis_entity *o);
static void send_update_explosion_packet(struct game_client *c,
	struct snis_entity *o);
static void send_update_torpedo_packet(struct game_client *c,
	struct snis_entity *o);
static void send_update_laser_packet(struct game_client *c,
	struct snis_entity *o);

static void queue_up_client_object_update(struct game_client *c, struct snis_entity *o)
{
	switch(o->type) {
	case OBJTYPE_SHIP1:
		send_update_ship_packet(c, o, OPCODE_UPDATE_SHIP);
		break;
	case OBJTYPE_SHIP2:
		send_econ_update_ship_packet(c, o);
		break;
	case OBJTYPE_PLANET:
		send_update_planet_packet(c, o);
		break;
	case OBJTYPE_STARBASE:
		send_update_starbase_packet(c, o);
		break;
	case OBJTYPE_EXPLOSION:
		send_update_explosion_packet(c, o);
		break;
	case OBJTYPE_DEBRIS:
		break;
	case OBJTYPE_SPARK:
		break;
	case OBJTYPE_TORPEDO:
		send_update_torpedo_packet(c, o);
		break;
	case OBJTYPE_LASER:
		send_update_laser_packet(c, o);
		break;
	default:
		break;
	}
}

static int too_far_away_to_care(struct game_client *c, struct snis_entity *o)
{
	struct snis_entity *ship = &go[c->shipid];
	double dx, dy, dist;
	const double threshold = (XUNIVERSE_DIMENSION / 2) * (XUNIVERSE_DIMENSION / 2);

	dx = (ship->x - o->x);
	dy = (ship->y - o->y);
	dist = (dx * dx) + (dy * dy);
	if (dist > threshold && snis_randn(100) < 70)
	 	return 1;	
	return 0;
}

static void queue_up_client_updates(struct game_client *c)
{
	int i;
	int count;

	count = 0;
	pthread_mutex_lock(&universe_mutex);
	for (i = 0; i <= snis_object_pool_highest_object(pool); i++) {
		/* printf("obj %d: a=%d, ts=%u, uts%u, type=%hhu\n",
			i, go[i].alive, go[i].timestamp, universe_timestamp, go[i].type); */
		if (go[i].alive && go[i].timestamp > c->timestamp) {
			if (too_far_away_to_care(c, &go[i]))
				continue;
			queue_up_client_object_update(c, &go[i]);
			count++;
		}
	}
	pthread_mutex_unlock(&universe_mutex);
	/* printf("queued up %d updates for client\n", count); */
}

static void queue_up_client_id(struct game_client *c)
{
	/* tell the client what his ship id is. */
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct update_ship_packet));
	packed_buffer_append_u16(pb, OPCODE_ID_CLIENT_SHIP);
	packed_buffer_append_u32(pb, c->shipid);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void *per_client_write_thread(__attribute__((unused)) void /* struct game_client */ *client)
{
	struct timespec time1;
	struct timespec time2;
	struct timespec tenth_second;
	int rc;
	struct game_client *c = (struct game_client *) client;

	memset(&tenth_second, 0, sizeof(tenth_second));
	tenth_second.tv_sec = 1;
	tenth_second.tv_nsec = 999999999;
	tenth_second.tv_nsec = 999999999;

	/* Wait for client[] array to get fully updated before proceeding. */
	client_lock();
	client_unlock();
	while (1) {
		rc = clock_gettime(CLOCK_MONOTONIC, &time1);
		queue_up_client_updates(c);
		write_queued_updates_to_client(c);
		if (c->socket < 0)
			break;
		c->timestamp = universe_timestamp;
		rc = clock_gettime(CLOCK_MONOTONIC, &time2);
		/* snis_sleep(&time1, &time2, &tenth_second); */ /* sleep for 1/10th sec - (time2 - time1) */
		sleep_tenth_second();
	}
	printf("client writer thread exiting.\n");
	if (rc) /* satisfy the whining compiler */
		return NULL;
	return NULL;
}

static int verify_client_protocol(int connection)
{
	int rc;
	char protocol_version[10];
	rc = snis_readsocket(connection, protocol_version, strlen(SNIS_PROTOCOL_VERSION));
	if (rc < 0)
		return rc;
	protocol_version[7] = '\0';
	printf("protocol read...'%s'\n", protocol_version);
	if (strcmp(protocol_version, SNIS_PROTOCOL_VERSION) != 0)
		return -1;
	printf("protocol verified.\n");
	return 0;
}

static int lookup_player(unsigned char *shipname, unsigned char *password)
{
	int i;

	pthread_mutex_lock(&universe_mutex);
	for (i = 0; i < nbridges; i++) {
		if (strcmp((const char *) shipname, (const char *) bridgelist[i].shipname) == 0 &&
			strcmp((const char *) password, (const char *) bridgelist[i].password) == 0) {
			pthread_mutex_unlock(&universe_mutex);
			return bridgelist[i].shipid;
		}
	}
	pthread_mutex_unlock(&universe_mutex);
	return -1;
}

static int insane(unsigned char *word, int len)
{
	int i;

	word[len-1] = '\0';
	len = strlen((const char *) word);
	for (i = 0; i < len; i++)
		if (!isalnum(word[i]))
			return 1;
	return 0;
}

static void send_econ_update_ship_packet(struct game_client *c,
	struct snis_entity *o)
{
	struct packed_buffer *pb;
	double dv;
	uint32_t x, y, v, heading;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	dv = sqrt((o->vx * o->vx) + (o->vy * o->vy));
	v = (int32_t) ((dv / XUNIVERSE_DIMENSION) * (double) INT32_MAX);
	heading = (uint32_t) (o->heading / 360.0 * (double) UINT32_MAX);

	pb = packed_buffer_allocate(sizeof(struct update_ship_packet));
	packed_buffer_append_u16(pb, OPCODE_ECON_UPDATE_SHIP);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, o->alive);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_append_u32(pb, v);
	packed_buffer_append_u32(pb, heading);

	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_ship_sdata_packet(struct game_client *c, struct ship_sdata_packet *sip)
{
	struct packed_buffer *pb;

	pb = packed_buffer_allocate(sizeof(struct ship_sdata_packet));
	packed_buffer_append_u16(pb, OPCODE_SHIP_SDATA);
	packed_buffer_append_u32(pb, sip->id); 
	packed_buffer_append_u8(pb, sip->subclass); 
	packed_buffer_append_raw(pb, sip->name, sizeof(sip->name));
	send_packet_to_all_clients_on_a_bridge(c->shipid, pb);
}
	
static void send_update_ship_packet(struct game_client *c,
	struct snis_entity *o, uint16_t opcode)
{
	struct packed_buffer *pb;
	uint32_t x, y;
	int32_t vx, vy;
	uint32_t heading, gun_heading, sci_heading, sci_beam_width, fuel;
	uint8_t tloading, tloaded, throttle, rpm;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	vx = (int32_t) ((o->vx / XUNIVERSE_DIMENSION) * (double) INT32_MAX);
	vy = (int32_t) ((o->vy / YUNIVERSE_DIMENSION) * (double) INT32_MAX);
	heading = (uint32_t) (o->heading / 360.0 * (double) UINT32_MAX);
	gun_heading = (uint32_t) (o->tsd.ship.gun_heading / 360.0 * (double) UINT32_MAX);
	sci_heading = (uint32_t) (o->tsd.ship.sci_heading / 360.0 * (double) UINT32_MAX);
	sci_beam_width = (uint32_t) (o->tsd.ship.sci_beam_width / 360.0 * (double) UINT32_MAX);
	throttle = o->tsd.ship.throttle;
	rpm = o->tsd.ship.rpm;
	fuel = o->tsd.ship.fuel;

	tloading = (uint8_t) (o->tsd.ship.torpedoes_loading & 0x0f);
	tloaded = (uint8_t) (o->tsd.ship.torpedoes_loaded & 0x0f);
	tloading = tloading | (tloaded << 4);

	pb = packed_buffer_allocate(sizeof(struct update_ship_packet));
	packed_buffer_append_u16(pb, opcode);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, o->alive);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_append_u32(pb, (uint32_t) vx);
	packed_buffer_append_u32(pb, (uint32_t) vy);
	packed_buffer_append_u32(pb, heading);
	packed_buffer_append_u32(pb, o->tsd.ship.torpedoes);
	packed_buffer_append_u32(pb, o->tsd.ship.power);
	packed_buffer_append_u32(pb, gun_heading);
	packed_buffer_append_u32(pb, sci_heading);
	packed_buffer_append_u32(pb, sci_beam_width);
	packed_buffer_append_u8(pb, tloading);
	packed_buffer_append_u8(pb, throttle);
	packed_buffer_append_u8(pb, rpm);
	packed_buffer_append_u32(pb, fuel);
	packed_buffer_append_u8(pb, o->tsd.ship.temp);
	packed_buffer_append_raw(pb, (char *) &o->tsd.ship.pwrdist, sizeof(o->tsd.ship.pwrdist));
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_update_planet_packet(struct game_client *c,
	struct snis_entity *o)
{
	struct packed_buffer *pb;
	uint32_t x, y;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	pb = packed_buffer_allocate(sizeof(struct update_planet_packet));
	packed_buffer_append_u16(pb, OPCODE_UPDATE_PLANET);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_update_starbase_packet(struct game_client *c,
	struct snis_entity *o)
{
	struct packed_buffer *pb;
	uint32_t x, y;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	pb = packed_buffer_allocate(sizeof(struct update_starbase_packet));
	packed_buffer_append_u16(pb, OPCODE_UPDATE_STARBASE);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_update_explosion_packet(struct game_client *c,
	struct snis_entity *o)
{
	struct packed_buffer *pb;
	uint32_t x, y;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	pb = packed_buffer_allocate(sizeof(struct update_starbase_packet));
	packed_buffer_append_u16(pb, OPCODE_UPDATE_EXPLOSION);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_append_u16(pb, o->tsd.explosion.nsparks);
	packed_buffer_append_u16(pb, o->tsd.explosion.velocity);
	packed_buffer_append_u16(pb, o->tsd.explosion.time);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_update_torpedo_packet(struct game_client *c,
	struct snis_entity *o)
{
	struct packed_buffer *pb;
	uint32_t x, y, vx, vy;

	x = (uint32_t) ((o->x / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	y = (uint32_t) ((o->y / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	vx = (uint32_t) ((o->vx / XUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	vy = (uint32_t) ((o->vy / YUNIVERSE_DIMENSION) * (double) UINT32_MAX);
	pb = packed_buffer_allocate(sizeof(struct update_torpedo_packet));
	packed_buffer_append_u16(pb, OPCODE_UPDATE_TORPEDO);
	packed_buffer_append_u32(pb, o->id);
	packed_buffer_append_u32(pb, o->tsd.torpedo.ship_id);
	packed_buffer_append_u32(pb, x);
	packed_buffer_append_u32(pb, y);
	packed_buffer_append_u32(pb, vx);
	packed_buffer_append_u32(pb, vy);
	packed_buffer_queue_add(&c->client_write_queue, pb, &c->client_write_queue_mutex);
}

static void send_update_laser_packet(struct game_client *c,
	struct snis_entity *o)
{
}

static int add_new_player(struct game_client *c)
{
	int rc;
	struct add_player_packet app;

	rc = snis_readsocket(c->socket, &app, sizeof(app));
	if (rc)
		return rc;
	app.opcode = ntohs(app.opcode);
	if (app.opcode != OPCODE_UPDATE_PLAYER) {
		printf("bad opcode %d\n", app.opcode);
		goto protocol_error;
	}
	app.shipname[19] = '\0';
	app.password[19] = '\0';
	if (app.role > ROLE_MAXROLE) {
		printf("server: role out of range: %d\n", app.role);
		goto protocol_error;
	}

	if (insane(app.shipname, 20) || insane(app.password, 20)) {
		printf("Bad ship name or password\n");
		goto protocol_error;
	}

	c->shipid = lookup_player(app.shipname, app.password);
	c->role = app.role;
	if (c->shipid == -1) { /* did not find our bridge, have to make a new one. */
		double x, y;

		x = XUNIVERSE_DIMENSION * (double) rand() / (double) RAND_MAX;
		y = YUNIVERSE_DIMENSION * (double) rand() / (double) RAND_MAX;
		pthread_mutex_lock(&universe_mutex);
		c->shipid = add_player(x, y, 0.0, 0.0, 0.0);
		strcpy(go[c->shipid].sdata.name, (const char * restrict) app.shipname);
		memset(&bridgelist[nbridges], 0, sizeof(bridgelist[nbridges]));
		strcpy((char *) bridgelist[nbridges].shipname, (const char *) app.shipname);
		strcpy((char *) bridgelist[nbridges].password, (const char *) app.password);
		bridgelist[nbridges].shipid = c->shipid;
	
		nbridges++;
		
		pthread_mutex_unlock(&universe_mutex);
	}
	queue_up_client_id(c);
	return 0;

protocol_error:
	printf("server: protocol error, closing socket %d\n", c->socket);
	close(c->socket);
	return -1;
}

/* Creates a thread for each incoming connection... */
static void service_connection(int connection)
{
	int i, flag = 1;

	printf("snis_server: servicing snis_client connection %d\n", connection);
        /* get connection moved off the stack so that when the thread needs it,
	 * it's actually still around. 
	 */

	i = setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
	if (i < 0)
		printf("setsockopt failed.\n");

	client_lock();
	if (nclients >= MAXCLIENTS) {
		client_unlock();
		printf("Too many clients.\n");
		return;
	}
	i = nclients;

	if (verify_client_protocol(connection)) {
		printf("protocol error\n");
		close(connection);
		return;
	}

	client[i].socket = connection;
	client[i].timestamp = 0;  /* newborn client, needs everything */

	printf("add new player\n");

	pthread_mutex_init(&client[i].client_write_queue_mutex, NULL);
	packed_buffer_queue_init(&client[i].client_write_queue);

	add_new_player(&client[i]);

	pthread_attr_init(&client[i].read_attr);
	pthread_attr_setdetachstate(&client[i].read_attr, PTHREAD_CREATE_JOINABLE);
	pthread_attr_init(&client[i].write_attr);
	pthread_attr_setdetachstate(&client[i].write_attr, PTHREAD_CREATE_JOINABLE);
        (void) pthread_create(&client[i].read_thread,
		&client[i].read_attr, per_client_read_thread, (void *) &client[i]);
        (void) pthread_create(&client[i].write_thread,
		&client[i].write_attr, per_client_write_thread, (void *) &client[i]);
	nclients++;
	client_unlock();


	printf("bottom of 'service connection'\n");
}

/* This thread listens for incoming client connections, and
 * on establishing a connection, starts a thread for that 
 * connection.
 */
static void *listener_thread(__attribute__((unused)) void * unused)
{
	int rendezvous, connection, rc;
        struct sockaddr_in remote_addr;
        socklen_t remote_addr_len;
	uint16_t port;
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int s;
	char portstr[20];

        printf("snis_server starting\n");

	/* Bind "rendezvous" socket to a random port to listen for connections. */
	while (1) {

		/* 
		 * choose a random port in the "Dynamic and/or Private" range
		 * see http://www.iana.org/assignments/port-numbers
		 */
		port = snis_randn(65335 - 49152) + 49151;
		printf("Trying port %d\n", port);
		sprintf(portstr, "%d", port);
		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;    /* For wildcard IP address */
		hints.ai_protocol = 0;          /* Any protocol */
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		s = getaddrinfo(NULL, portstr, &hints, &result);
		if (s != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(s));
			exit(EXIT_FAILURE);
		}

		/* getaddrinfo() returns a list of address structures.
		 * Try each address until we successfully bind(2).
		 * If socket(2) (or bind(2)) fails, we (close the socket
		 * and) try the next address. 
		 */

		for (rp = result; rp != NULL; rp = rp->ai_next) {
			rendezvous = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (rendezvous == -1)
				continue;

			if (bind(rendezvous, rp->ai_addr, rp->ai_addrlen) == 0)
				break;                  /* Success */
			close(rendezvous);
		}
		if (rp != NULL)
			break;
	}

	/* At this point, "rendezvous" is bound to a random port */
	printf("snis_server listening for connections on port %d\n", port);
	listener_port = port;

	/* Listen for incoming connections... */
	rc = listen(rendezvous, SOMAXCONN);
	if (rc < 0) {
		fprintf(stderr, "listen() failed: %s\n", strerror(errno));
		exit(1);
	}

	/* Notify other threads that the listener thread is ready... */
	(void) pthread_mutex_lock(&listener_mutex);
	pthread_cond_signal(&listener_started);
	(void) pthread_mutex_unlock(&listener_mutex);

	while (1) {
		remote_addr_len = sizeof(remote_addr);
		printf("Accepting connection...\n");
		connection = accept(rendezvous, (struct sockaddr *) &remote_addr, &remote_addr_len);
		printf("accept returned %d\n", connection);
		if (connection < 0) {
			/* handle failed connection... */
			fprintf(stderr, "accept() failed: %s\n", strerror(errno));
			ssgl_sleep(1);
			continue;
		}
		if (remote_addr_len != sizeof(remote_addr)) {
			fprintf(stderr, "strange socket address length %d\n", remote_addr_len);
			/* shutdown(connection, SHUT_RDWR);
			close(connection);
			continue; */
		}
		service_connection(connection);
	}
}

/* Starts listener thread to listen for incoming client connections.
 * Returns port on which listener thread is listening. 
 */
static int start_listener_thread(void)
{
	pthread_attr_t attr;
	pthread_t thread;

	/* Setup to wait for the listener thread to become ready... */
	pthread_cond_init (&listener_started, NULL);
        (void) pthread_mutex_lock(&listener_mutex);

	/* Create the listener thread... */
        pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
        (void) pthread_create(&thread, &attr, listener_thread, NULL);

	/* Wait for the listener thread to become ready... */
	pthread_cond_wait(&listener_started, &listener_mutex);
	(void) pthread_mutex_unlock(&listener_mutex);
	printf("Listener started.\n");
	
	return listener_port;
}


static void move_objects(void)
{
	int i;

	pthread_mutex_lock(&universe_mutex);
	universe_timestamp++;
	for (i = 0; i <= snis_object_pool_highest_object(pool); i++)
		if (go[i].alive)
			go[i].move(&go[i]);
	pthread_mutex_unlock(&universe_mutex);

}

static void register_with_game_lobby(int port,
	char *servernick, char *gameinstance, char *location)
{
	struct ssgl_game_server gs;

	memset(&gs, 0, sizeof(gs));
	printf("port = %hu\n", port);
	gs.port = htons(port);
	printf("gs.port = %hu\n", gs.port);
		
	strncpy(gs.server_nickname, servernick, 14);
	strncpy(gs.game_instance, gameinstance, 19);
	strncpy(gs.location, location, 19);
	strcpy(gs.game_type, "SNIS");

	if (ssgl_get_primary_host_ip_addr(&gs.ipaddr) != 0)
		fprintf(stderr, "Failed to get local ip address.\n");

	printf("Registering game server\n");
#define LOBBYHOST "localhost"
	(void) ssgl_register_gameserver(LOBBYHOST, &gs, &lobbythread);
	printf("Game server registered.\n");
	return;	
}

void usage(void)
{
	fprintf(stderr, "snis_server gameinstance servernick location\n");
	fprintf(stderr, "For example: snis_server 'steves game' zuul Houston\n");
	exit(0);
}

int main(int argc, char *argv[])
{
	int port, rc, i;
	struct timespec time1;
	struct timespec time2;
	struct timespec thirtieth_second;

	if (argc < 4) 
		usage();

	memset(&thirtieth_second, 0, sizeof(thirtieth_second));
	thirtieth_second.tv_nsec = 33333333; /* 1/30th second */

	make_universe();
	port = start_listener_thread();

	ignore_sigpipe();	
	register_with_game_lobby(port, argv[2], argv[1], argv[3]);

	i = 0;
	while (1) {
		rc = clock_gettime(CLOCK_MONOTONIC, &time1);
		/* if ((i % 30) == 0) printf("Moving objects...i = %d\n", i); */
		i++;
		move_objects();
		rc = clock_gettime(CLOCK_MONOTONIC, &time2);
		/* snis_sleep(&time1, &time2, &thirtieth_second); */
		sleep_thirtieth_second();
	}

	if (rc) /* satisfy compiler */
		return 0; 
	return 0;
}
