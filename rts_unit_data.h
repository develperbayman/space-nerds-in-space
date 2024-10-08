/*
	Copyright (C) 2017 Stephen M. Cameron
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
#ifndef SNIS_RTS_UNIT_DATA_H_
#define SNIS_RTS_UNIT_DATA_H_

#include <stdlib.h>

#define NUM_RTS_UNIT_TYPES 6
#define NUM_RTS_ORDER_TYPES 8

#define RTS_UNIT_SCOUT_SHIP 0
#define RTS_UNIT_HEAVY_BOMBER 1
#define RTS_UNIT_GUN_SHIP 2
#define RTS_UNIT_TROOP_SHIP 3
#define RTS_UNIT_TURRET 4
#define RTS_UNIT_RESUPPLY_SHIP 5

struct rts_unit_data {
	char *name;
	char *class; /* Name of ship model to use, matches snis_ship_type.h ->class */
	char *short_name_prefix;
	float cost_to_build;
	int time_to_build; /* in 10ths of secs */
	float toughness;
	float damage_factor;
	float fuel_capacity;
	float fuel_consumption_unit;
};

#define RTS_ORDERS_STAND_BY 0
#define RTS_ORDERS_GUARD_BASE 1
#define RTS_ORDERS_ESCORT 2
#define RTS_ORDERS_ATK_NEAR_ENEMY 3
#define RTS_ORDERS_MOVE_TO_WAYPOINT 4
#define RTS_ORDERS_OCCUPY_NEAR_BASE 5
#define RTS_ORDERS_ATK_MAIN_BASE 6
#define RTS_ORDERS_RESUPPLY 7
#define RTS_ORDERS_OUT_OF_FUEL 8

struct rts_order_data {
	char *name;
	char *short_name;
	char *help_text;
	float cost_to_order;
	int user_selectable;
};

struct rts_unit_data *rts_unit_type(int n);
struct rts_order_data *rts_order_type(int n);
int rts_allocate_unit_number(int unit_type, int faction);
int rts_unit_type_to_ship_type(int unit_type);
void set_rts_unit_type_to_ship_type(int unit_type, int ship_type);
int orders_valid_for_unit_type(int orders, int unit_type);

#endif

