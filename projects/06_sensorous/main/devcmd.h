#pragma once

/*
 * Single-character commands on the USB console, for driving the board from
 * the desk without touching it. Read by a small task; the app polls.
 *   m  toggle maintenance mode      r  reload the book from the card
 *   s  toggle sleep                 i  print a status line
 *   d  toggle DEBUG on every tag    (own tags are DEBUG already)
 */
void devcmd_init(void);

/* The next pending command, or 0. */
char devcmd_take(void);
