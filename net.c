/*
 * net.c: Net game.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "puzzles.h"
#include "tree234.h"

/* Direction bitfields */
#define R 0x01
#define U 0x02
#define L 0x04
#define D 0x08
#define LOCKED 0x10

/* Rotations: Anticlockwise, Clockwise, Flip, general rotate */
#define A(x) ( (((x) & 0x07) << 1) | (((x) & 0x08) >> 3) )
#define C(x) ( (((x) & 0x0E) >> 1) | (((x) & 0x01) << 3) )
#define F(x) ( (((x) & 0x0C) >> 2) | (((x) & 0x03) << 2) )
#define ROT(x, n) ( ((n)&3) == 0 ? (x) : \
		    ((n)&3) == 1 ? A(x) : \
		    ((n)&3) == 2 ? F(x) : C(x) )

/* X and Y displacements */
#define X(x) ( (x) == R ? +1 : (x) == L ? -1 : 0 )
#define Y(x) ( (x) == D ? +1 : (x) == U ? -1 : 0 )

/* Bit count */
#define COUNT(x) ( (((x) & 0x08) >> 3) + (((x) & 0x04) >> 2) + \
		   (((x) & 0x02) >> 1) + ((x) & 0x01) )

#define TILE_SIZE 32
#define TILE_BORDER 1
#define WINDOW_OFFSET 16

struct game_params {
    int width;
    int height;
    int wrapping;
    float barrier_probability;
};

struct game_state {
    int width, height, wrapping, completed;
    unsigned char *tiles;
    unsigned char *barriers;
};

#define OFFSET(x2,y2,x1,y1,dir,state) \
    ( (x2) = ((x1) + (state)->width + X((dir))) % (state)->width, \
      (y2) = ((y1) + (state)->height + Y((dir))) % (state)->height)

#define index(state, a, x, y) ( a[(y) * (state)->width + (x)] )
#define tile(state, x, y)     index(state, (state)->tiles, x, y)
#define barrier(state, x, y)  index(state, (state)->barriers, x, y)

struct xyd {
    int x, y, direction;
};

static int xyd_cmp(void *av, void *bv) {
    struct xyd *a = (struct xyd *)av;
    struct xyd *b = (struct xyd *)bv;
    if (a->x < b->x)
	return -1;
    if (a->x > b->x)
	return +1;
    if (a->y < b->y)
	return -1;
    if (a->y > b->y)
	return +1;
    if (a->direction < b->direction)
	return -1;
    if (a->direction > b->direction)
	return +1;
    return 0;
};

static struct xyd *new_xyd(int x, int y, int direction)
{
    struct xyd *xyd = snew(struct xyd);
    xyd->x = x;
    xyd->y = y;
    xyd->direction = direction;
    return xyd;
}

/* ----------------------------------------------------------------------
 * Randomly select a new game seed.
 */

char *new_game_seed(game_params *params)
{
    /*
     * The full description of a Net game is far too large to
     * encode directly in the seed, so by default we'll have to go
     * for the simple approach of providing a random-number seed.
     * 
     * (This does not restrict me from _later on_ inventing a seed
     * string syntax which can never be generated by this code -
     * for example, strings beginning with a letter - allowing me
     * to type in a precise game, and have new_game detect it and
     * understand it and do something completely different.)
     */
    char buf[40];
    sprintf(buf, "%d", rand());
    return dupstr(buf);
}

/* ----------------------------------------------------------------------
 * Construct an initial game state, given a seed and parameters.
 */

game_state *new_game(game_params *params, char *seed)
{
    random_state *rs;
    game_state *state;
    tree234 *possibilities, *barriers;
    int w, h, x, y, nbarriers;

    assert(params->width > 2);
    assert(params->height > 2);

    /*
     * Create a blank game state.
     */
    state = snew(game_state);
    w = state->width = params->width;
    h = state->height = params->height;
    state->wrapping = params->wrapping;
    state->completed = FALSE;
    state->tiles = snewn(state->width * state->height, unsigned char);
    memset(state->tiles, 0, state->width * state->height);
    state->barriers = snewn(state->width * state->height, unsigned char);
    memset(state->barriers, 0, state->width * state->height);

    /*
     * Set up border barriers if this is a non-wrapping game.
     */
    if (!state->wrapping) {
	for (x = 0; x < state->width; x++) {
	    barrier(state, x, 0) |= U;
	    barrier(state, x, state->height-1) |= D;
	}
	for (y = 0; y < state->height; y++) {
	    barrier(state, y, 0) |= L;
	    barrier(state, y, state->width-1) |= R;
	}
    }

    /*
     * Seed the internal random number generator.
     */
    rs = random_init(seed, strlen(seed));

    /*
     * Construct the unshuffled grid.
     * 
     * To do this, we simply start at the centre point, repeatedly
     * choose a random possibility out of the available ways to
     * extend a used square into an unused one, and do it. After
     * extending the third line out of a square, we remove the
     * fourth from the possibilities list to avoid any full-cross
     * squares (which would make the game too easy because they
     * only have one orientation).
     * 
     * The slightly worrying thing is the avoidance of full-cross
     * squares. Can this cause our unsophisticated construction
     * algorithm to paint itself into a corner, by getting into a
     * situation where there are some unreached squares and the
     * only way to reach any of them is to extend a T-piece into a
     * full cross?
     * 
     * Answer: no it can't, and here's a proof.
     * 
     * Any contiguous group of such unreachable squares must be
     * surrounded on _all_ sides by T-pieces pointing away from the
     * group. (If not, then there is a square which can be extended
     * into one of the `unreachable' ones, and so it wasn't
     * unreachable after all.) In particular, this implies that
     * each contiguous group of unreachable squares must be
     * rectangular in shape (any deviation from that yields a
     * non-T-piece next to an `unreachable' square).
     * 
     * So we have a rectangle of unreachable squares, with T-pieces
     * forming a solid border around the rectangle. The corners of
     * that border must be connected (since every tile connects all
     * the lines arriving in it), and therefore the border must
     * form a closed loop around the rectangle.
     * 
     * But this can't have happened in the first place, since we
     * _know_ we've avoided creating closed loops! Hence, no such
     * situation can ever arise, and the naive grid construction
     * algorithm will guaranteeably result in a complete grid
     * containing no unreached squares, no full crosses _and_ no
     * closed loops. []
     */
    possibilities = newtree234(xyd_cmp);
    add234(possibilities, new_xyd(w/2, h/2, R));
    add234(possibilities, new_xyd(w/2, h/2, U));
    add234(possibilities, new_xyd(w/2, h/2, L));
    add234(possibilities, new_xyd(w/2, h/2, D));

    while (count234(possibilities) > 0) {
	int i;
	struct xyd *xyd;
	int x1, y1, d1, x2, y2, d2, d;

	/*
	 * Extract a randomly chosen possibility from the list.
	 */
	i = random_upto(rs, count234(possibilities));
	xyd = delpos234(possibilities, i);
	x1 = xyd->x;
	y1 = xyd->y;
	d1 = xyd->direction;
	sfree(xyd);

	OFFSET(x2, y2, x1, y1, d1, state);
	d2 = F(d1);
#ifdef DEBUG
	printf("picked (%d,%d,%c) <-> (%d,%d,%c)\n",
	       x1, y1, "0RU3L567D9abcdef"[d1], x2, y2, "0RU3L567D9abcdef"[d2]);
#endif

	/*
	 * Make the connection. (We should be moving to an as yet
	 * unused tile.)
	 */
	tile(state, x1, y1) |= d1;
	assert(tile(state, x2, y2) == 0);
	tile(state, x2, y2) |= d2;

	/*
	 * If we have created a T-piece, remove its last
	 * possibility.
	 */
	if (COUNT(tile(state, x1, y1)) == 3) {
	    struct xyd xyd1, *xydp;

	    xyd1.x = x1;
	    xyd1.y = y1;
	    xyd1.direction = 0x0F ^ tile(state, x1, y1);

	    xydp = find234(possibilities, &xyd1, NULL);

	    if (xydp) {
#ifdef DEBUG
		printf("T-piece; removing (%d,%d,%c)\n",
		       xydp->x, xydp->y, "0RU3L567D9abcdef"[xydp->direction]);
#endif
		del234(possibilities, xydp);
		sfree(xydp);
	    }
	}

	/*
	 * Remove all other possibilities that were pointing at the
	 * tile we've just moved into.
	 */
	for (d = 1; d < 0x10; d <<= 1) {
	    int x3, y3, d3;
	    struct xyd xyd1, *xydp;

	    OFFSET(x3, y3, x2, y2, d, state);
	    d3 = F(d);

	    xyd1.x = x3;
	    xyd1.y = y3;
	    xyd1.direction = d3;

	    xydp = find234(possibilities, &xyd1, NULL);

	    if (xydp) {
#ifdef DEBUG
		printf("Loop avoidance; removing (%d,%d,%c)\n",
		       xydp->x, xydp->y, "0RU3L567D9abcdef"[xydp->direction]);
#endif
		del234(possibilities, xydp);
		sfree(xydp);
	    }
	}

	/*
	 * Add new possibilities to the list for moving _out_ of
	 * the tile we have just moved into.
	 */
	for (d = 1; d < 0x10; d <<= 1) {
	    int x3, y3;

	    if (d == d2)
		continue;	       /* we've got this one already */

	    if (!state->wrapping) {
		if (d == U && y2 == 0)
		    continue;
		if (d == D && y2 == state->height-1)
		    continue;
		if (d == L && x2 == 0)
		    continue;
		if (d == R && x2 == state->width-1)
		    continue;
	    }

	    OFFSET(x3, y3, x2, y2, d, state);

	    if (tile(state, x3, y3))
		continue;	       /* this would create a loop */

#ifdef DEBUG
	    printf("New frontier; adding (%d,%d,%c)\n",
		   x2, y2, "0RU3L567D9abcdef"[d]);
#endif
	    add234(possibilities, new_xyd(x2, y2, d));
	}
    }
    /* Having done that, we should have no possibilities remaining. */
    assert(count234(possibilities) == 0);
    freetree234(possibilities);

    /*
     * Now compute a list of the possible barrier locations.
     */
    barriers = newtree234(xyd_cmp);
    for (y = 0; y < state->height - (!state->wrapping); y++) {
	for (x = 0; x < state->width - (!state->wrapping); x++) {

	    if (!(tile(state, x, y) & R))
		add234(barriers, new_xyd(x, y, R));
	    if (!(tile(state, x, y) & D))
		add234(barriers, new_xyd(x, y, D));
	}
    }

    /*
     * Now shuffle the grid.
     */
    for (y = 0; y < state->height - (!state->wrapping); y++) {
	for (x = 0; x < state->width - (!state->wrapping); x++) {
	    int orig = tile(state, x, y);
	    int rot = random_upto(rs, 4);
	    tile(state, x, y) = ROT(orig, rot);
	}
    }

    /*
     * And now choose barrier locations. (We carefully do this
     * _after_ shuffling, so that changing the barrier rate in the
     * params while keeping the game seed the same will give the
     * same shuffled grid and _only_ change the barrier locations.
     * Also the way we choose barrier locations, by repeatedly
     * choosing one possibility from the list until we have enough,
     * is designed to ensure that raising the barrier rate while
     * keeping the seed the same will provide a superset of the
     * previous barrier set - i.e. if you ask for 10 barriers, and
     * then decide that's still too hard and ask for 20, you'll get
     * the original 10 plus 10 more, rather than getting 20 new
     * ones and the chance of remembering your first 10.)
     */
    nbarriers = params->barrier_probability * count234(barriers);
    assert(nbarriers >= 0 && nbarriers <= count234(barriers));

    while (nbarriers > 0) {
	int i;
	struct xyd *xyd;
	int x1, y1, d1, x2, y2, d2;

	/*
	 * Extract a randomly chosen barrier from the list.
	 */
	i = random_upto(rs, count234(barriers));
	xyd = delpos234(barriers, i);

	assert(xyd != NULL);

	x1 = xyd->x;
	y1 = xyd->y;
	d1 = xyd->direction;
	sfree(xyd);

	OFFSET(x2, y2, x1, y1, d1, state);
	d2 = F(d1);

	barrier(state, x1, y1) |= d1;
	barrier(state, x2, y2) |= d2;

	nbarriers--;
    }

    /*
     * Clean up the rest of the barrier list.
     */
    {
	struct xyd *xyd;

	while ( (xyd = delpos234(barriers, 0)) != NULL)
	    sfree(xyd);

	freetree234(barriers);
    }

    random_free(rs);

    return state;
}

game_state *dup_game(game_state *state)
{
    game_state *ret;

    ret = snew(game_state);
    ret->width = state->width;
    ret->height = state->height;
    ret->wrapping = state->wrapping;
    ret->completed = state->completed;
    ret->tiles = snewn(state->width * state->height, unsigned char);
    memcpy(ret->tiles, state->tiles, state->width * state->height);
    ret->barriers = snewn(state->width * state->height, unsigned char);
    memcpy(ret->barriers, state->barriers, state->width * state->height);

    return ret;
}

void free_game(game_state *state)
{
    sfree(state->tiles);
    sfree(state->barriers);
    sfree(state);
}

/* ----------------------------------------------------------------------
 * Utility routine.
 */

/*
 * Compute which squares are reachable from the centre square, as a
 * quick visual aid to determining how close the game is to
 * completion. This is also a simple way to tell if the game _is_
 * completed - just call this function and see whether every square
 * is marked active.
 */
static unsigned char *compute_active(game_state *state)
{
    unsigned char *active;
    tree234 *todo;
    struct xyd *xyd;

    active = snewn(state->width * state->height, unsigned char);
    memset(active, 0, state->width * state->height);

    /*
     * We only store (x,y) pairs in todo, but it's easier to reuse
     * xyd_cmp and just store direction 0 every time.
     */
    todo = newtree234(xyd_cmp);
    add234(todo, new_xyd(state->width / 2, state->height / 2, 0));

    while ( (xyd = delpos234(todo, 0)) != NULL) {
	int x1, y1, d1, x2, y2, d2;

	x1 = xyd->x;
	y1 = xyd->y;
	sfree(xyd);

	for (d1 = 1; d1 < 0x10; d1 <<= 1) {
	    OFFSET(x2, y2, x1, y1, d1, state);
	    d2 = F(d1);

	    /*
	     * If the next tile in this direction is connected to
	     * us, and there isn't a barrier in the way, and it
	     * isn't already marked active, then mark it active and
	     * add it to the to-examine list.
	     */
	    if ((tile(state, x1, y1) & d1) &&
		(tile(state, x2, y2) & d2) &&
		!(barrier(state, x1, y1) & d1) &&
		!index(state, active, x2, y2)) {
		index(state, active, x2, y2) = 1;
		add234(todo, new_xyd(x2, y2, 0));
	    }
	}
    }
    /* Now we expect the todo list to have shrunk to zero size. */
    assert(count234(todo) == 0);
    freetree234(todo);

    return active;
}

/* ----------------------------------------------------------------------
 * Process a move.
 */
game_state *make_move(game_state *state, int x, int y, int button)
{
    game_state *ret;
    int tx, ty, orig;

    /*
     * All moves in Net are made with the mouse.
     */
    if (button != LEFT_BUTTON &&
	button != MIDDLE_BUTTON &&
	button != RIGHT_BUTTON)
	return NULL;

    /*
     * The button must have been clicked on a valid tile.
     */
    x -= WINDOW_OFFSET;
    y -= WINDOW_OFFSET;
    if (x < 0 || y < 0)
	return NULL;
    tx = x / TILE_SIZE;
    ty = y / TILE_SIZE;
    if (tx >= state->width || ty >= state->height)
	return NULL;
    if (tx % TILE_SIZE >= TILE_SIZE - TILE_BORDER ||
	ty % TILE_SIZE >= TILE_SIZE - TILE_BORDER)
	return NULL;

    /*
     * The middle button locks or unlocks a tile. (A locked tile
     * cannot be turned, and is visually marked as being locked.
     * This is a convenience for the player, so that once they are
     * sure which way round a tile goes, they can lock it and thus
     * avoid forgetting later on that they'd already done that one;
     * and the locking also prevents them turning the tile by
     * accident. If they change their mind, another middle click
     * unlocks it.)
     */
    if (button == MIDDLE_BUTTON) {
	ret = dup_game(state);
	tile(ret, tx, ty) ^= LOCKED;
	return ret;
    }

    /*
     * The left and right buttons have no effect if clicked on a
     * locked tile.
     */
    if (tile(state, tx, ty) & LOCKED)
	return NULL;

    /*
     * Otherwise, turn the tile one way or the other. Left button
     * turns anticlockwise; right button turns clockwise.
     */
    ret = dup_game(state);
    orig = tile(ret, tx, ty);
    if (button == LEFT_BUTTON)
	tile(ret, tx, ty) = A(orig);
    else
	tile(ret, tx, ty) = C(orig);

    /*
     * Check whether the game has been completed.
     */
    {
	unsigned char *active = compute_active(ret);
	int x1, y1;
	int complete = TRUE;

	for (x1 = 0; x1 < ret->width; x1++)
	    for (y1 = 0; y1 < ret->height; y1++)
		if (!index(ret, active, x1, y1)) {
		    complete = FALSE;
		    goto break_label;  /* break out of two loops at once */
		}
	break_label:

	sfree(active);

	if (complete)
	    ret->completed = TRUE;
    }

    return ret;
}

/* ----------------------------------------------------------------------
 * Routines for drawing the game position on the screen.
 */

#ifndef TESTMODE		       /* FIXME: should be #ifdef */

int main(void)
{
    game_params params = { 13, 11, TRUE, 0.1 };
    char *seed;
    game_state *state;
    unsigned char *active;

    seed = "123";
    state = new_game(&params, seed);
    active = compute_active(state);

    {
	int x, y;

	printf("\033)0\016");
	for (y = 0; y < state->height; y++) {
	    for (x = 0; x < state->width; x++) {
		if (index(state, active, x, y))
		    printf("\033[1;32m");
		else
		    printf("\033[0;31m");
		putchar("~``m`qjv`lxtkwua"[tile(state, x, y)]);
	    }
	    printf("\033[m\n");
	}
	printf("\017");
    }

    free_game(state);

    return 0;
}

#endif
