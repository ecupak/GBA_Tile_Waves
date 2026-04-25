#include <tonc.h>
#include <stdlib.h>

#define CBB_0 0
#define SBB_8 8
#define SBB_9 9

#define BANK_R 0b0001
#define BANK_G 0b0010
#define BANK_B 0b0100
#define BANK_Y (BANK_R | BANK_G)
#define BANK_M (BANK_R | BANK_B)
#define BANK_C (BANK_G | BANK_B)

#define BANK_OVERFLOW 4

// Every color has 4 shades of intensity.
#define SHADE_COUNT 4

#define GREEN_SWAP (*(volatile u16*)0x04000002)

#define MAX_WAVEFRONTS 10


// Window boundary (as tiles) for the waves.
struct WTILEBOUNDS
{
	u16 min_tile_x;
	u16 max_tile_x;
	u16 min_tile_y;
	u16 max_tile_y;
};


// Wavefront origin.
struct ORIGIN
{
	u16 x;
	u16 y;
};


// Controls wavefront values over time.
struct CONTROL_DELTA
{
	u16 amount;
	u16 delta;
	u16 delay;
	u16 timer;
};


// Wavefront (expanding circle).
struct WAVEFRONT
{
	u16 palette_bank;
	struct ORIGIN origin;
	struct CONTROL_DELTA radius;
	//struct CONTROL_DELTA intensity;
};


// Wavefont method. Resets a controller.
void reset_control_delta(struct CONTROL_DELTA *cd, u16 default_amount)
{
	cd->amount = default_amount;
	cd->timer = 0;
}


// Wavefront method. Called to advance control timer. When timer reaches delay value, add delta to amount.
// Returs if the timer was reset or not (can be used to time other effects).
bool advance_control_delta(struct CONTROL_DELTA *cd)
{
	if (++(cd->timer) >= cd->delay)
	{
		cd->amount += cd->delta;
		cd->timer = 0;

		return true;
	}

	return false;
}


// Shifts red 0 bits, green 5 bits, blue 10 bits.
u16 get_bank_shifted_color(u16 color, u16 bank)
{
	return color << ((bank >> 1) * 5);
}


// Creates new bank with combinations of all shades from 2 other banks.
// Lookup into the mixed bank will use index of the colors as (idx_x + idx_y * shade_count).
// Lower bank will always be the x value (red if with green or blue; green if with blue).
void mix_palettes(u16 x_palette_bank, u16 y_palette_bank)
{
	const u16 mixed_palette_bank = x_palette_bank | y_palette_bank;
	
	// We start filling the new bank at index 1 because 0 is reserved for black/transparent color.
	u16 index = 1;

	// Start both loop indices at 1 because 0 is reserved for black/transparent color.
	for (u16 y = 1; y <= SHADE_COUNT; ++y)
	{
		u16 y_col = pal_bg_mem[y + (16 * y_palette_bank)];
		//u16 y_col = get_bank_shifted_color(y_val, y_palette_bank);

		for (u16 x = 1; x <= SHADE_COUNT; ++x)
		{
			u16 x_col = pal_bg_mem[x + (16 * x_palette_bank)];
			//u16 x_col = get_bank_shifted_color(x_val, x_palette_bank);
			
			// Create color.
			u16 color = x_col | y_col;

			// Store color.
			// There are 4 shades, so there are 16 combinations. And since a bank has 16 values, this seems great.
			// But index 0 is reserved for black/transparent color, so we place the first color in a different bank.
			if (y == 1 && x == 1) 
			{
				pal_bg_mem[index + (16 * (mixed_palette_bank + BANK_OVERFLOW))] = color;
			}
			else
			{
				pal_bg_mem[index + (16 * mixed_palette_bank)] = color;
				++index;
			}
		}
	}
}


// Programmatically create color palette.
// Palette has primary colors in banks 1, 2, 4.
// Allows placing mixed colors in the bitwise-OR banks of 3, 5, 6.
// Index 0 of each bank must be black/transparent.
void load_colors()
{
	// First 2 blocks in bank 0 are black and white. Used for general bg color and window color.
	pal_bg_mem[0] = RGB15(0, 0, 0);
	pal_bg_mem[1] = RGB15(31, 31, 31);

	// Setup the primary colors in banks 1, 2, 4 (0b0001, 0b0010, 0b0100).
	// Colors are added in increasing intensity.
	const float max_value = 31.0f;
	const float color_frac = max_value / (float)(SHADE_COUNT);

	for (u16 i = 0; i <= SHADE_COUNT; ++i)
	{
		u32 color = (u32)(min((float)(i) * color_frac, max_value));
		
		pal_bg_mem[i + (16 * BANK_R)] = RGB15(color, 0, 0); // R
		pal_bg_mem[i + (16 * BANK_G)] = RGB15(0, color, 0); // G
		pal_bg_mem[i + (16 * BANK_B)] = RGB15(0, 0, color); // B
	}

	// Create mixed colors (yellow, magenta, cyan) in banks 3, 5, 6.
	// ... Red + Green = Yellow
	mix_palettes(BANK_R, BANK_G);
	mix_palettes(BANK_R, BANK_B);
	mix_palettes(BANK_G, BANK_B);
}


// Programmatically create the tiles.
void load_tiles()
{
	// First tile is left blank (all 0's).
	// Don't need to actually assign it, it is this by default.
	// tile_mem[CBB_0][0] = 0;

	// Create tiles for the wavefront.

	// ... Solid block with bottom and right edge cut-away:
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 1 1 1 1 1 1 1 0
	// 0 0 0 0 0 0 0 0

	// ... Each block uses a single palette index.
	// ... To show varying shades of a color in the same bank, change the tile id to that palette index.
	u16 i = 1;
	u16 max_i = 0x10;
	for(; i < max_i; ++i)
	{	
		TILE block;
		for (u16 ii = 0; ii < 7; ++ii)
		{
			block.data[ii] = 0x01111111u * i;
		}
		block.data[7] = 0x00u;
		
		tile_mem[CBB_0][i] = block;
	}

	// Create tiles for the wavefront window border.
	// Will flip them around to complete design.

	// ... Top edge.
	{
		TILE block;
		block.data[0] = 0x11111111u;
		tile_mem[CBB_0][i++] = block;
	}

	// ... Left edge.
	{
		TILE block;
		for (u16 ii = 0; ii < 8; ++ii)
		{
			block.data[ii] = 0x00000001u;
		}
		tile_mem[CBB_0][i++] = block;
	}

	// ... Corner piece.
	{
		TILE block;
		block.data[0] = 0x11111111u;
		for (u16 ii = 1; ii < 8; ++ii)
		{
			block.data[ii] = 0x00000001u;
		}
		tile_mem[CBB_0][i++] = block;
	}

}


u16 get_overlapping_wave_details(u16 x_pos, u16 y_pos, struct WAVEFRONT wavefront)
{
	static const u16 base_distance = 4;
	static const u16 distance_increment = 2;
	static const u16 max_distance_threshold = base_distance + (SHADE_COUNT * distance_increment) - distance_increment;
					
	// Get component distance to the origin from SE.
	u16 x_dist = ABS(x_pos - wavefront.origin.x);
	u16 y_dist = ABS(y_pos - wavefront.origin.y);

	// Use octagonal distance formula.
	// ... Precalculated 3/8 = 0.375f
	// ... NOTE: Using approximation without float is much faster.
	// ... TODO: Figure out fixed-point math.
	//u16 dist_to_origin = (u16)((1.0f * max(x_dist, y_dist)) + (0.375f * min(x_dist, y_dist)));
	u16 dist_to_origin = max(x_dist, y_dist) + (min(x_dist, y_dist) >> 1);
	
	// Compare radius of wave to tile distance to wave origin to find distance to edge.
	u16 dist_to_edge = ABS(wavefront.radius.amount - dist_to_origin);

	// Skip if no wave overlap.
	if (dist_to_edge >= max_distance_threshold)
		return 0;
	
	// Map distance to edge with diminishing color value.	
	for (u16 i = 0; i < SHADE_COUNT; ++i)
	{
		u16 threshold_distance = base_distance + (distance_increment * i);
		
		if (dist_to_edge < threshold_distance)
		{
			return SHADE_COUNT - i;
		}
	}

	// Because compiler warnings.
	return 0;
}


// Combine banks of the overlapping waves, then index into correct shade (out of 16).
SCR_ENTRY get_attenuated_palette_index(u16 *overlapping_palette_banks, u16 *overlapping_palette_indxs, u16 overlapping_palette_size)
{
	// Only supports 2 overlapping waves right now.
	static const u16 wave_count = 2;

	// Merge banks to find mix.
	u16 bank = 0;
	for (u16 i = 0; i < wave_count; ++i)
		bank |= overlapping_palette_banks[i];

	// If the mixed bank is same as 1 of the original banks, then both waves are same color/bank.
	// Just use the more intense value of that color.
	if (bank == overlapping_palette_banks[0])
		return SE_PALBANK(bank) | max(overlapping_palette_indxs[0], overlapping_palette_indxs[1]);

	// If they're different, we calculate the mixed index.
	// As mentioned when creating the mixed palette, we find the index with (x + y * SHADE_COUNT).

	// We always use the lower bank as x.
	u16 x_val = overlapping_palette_banks[0] < overlapping_palette_banks[1] ? overlapping_palette_indxs[0] : overlapping_palette_indxs[1];
	u16 y_val = overlapping_palette_banks[0] < overlapping_palette_banks[1] ? overlapping_palette_indxs[1] : overlapping_palette_indxs[0];
	
	// Because the mixed banks start at index 0 (unlike the primary banks, which start at 1), we need to offset the values down by 1.
	x_val -= 1;
	y_val -= 1;
	
	// Find the index of the tile that points to the correct blend.
	u16 tile_index = x_val + (y_val * SHADE_COUNT);

	// If index would be 0, activate index 1 of offset bank.
	// Even though the mixed colors have 16 values, index 0 has to be empty for the transparent pixels on each block. 
	// So earlier we placed that first color in an offset bank at index 1.
	if (tile_index == 0)
		return SE_PALBANK(bank + BANK_OVERFLOW) | 0x01;
	else
		return SE_PALBANK(bank) | tile_index;
}

		
// Every tile compares its distance to every wave and sets its palette bank and index accordingly.
void update_tiles(struct WAVEFRONT *wavefronts, u16 wavefront_size, struct WTILEBOUNDS wbounds)
{
	// Number of tiles across in BG_SIZE(0) background.
	const u16 screen_tile_pitch = 32;

	// It would be unlikely that any single tile would be overlapped by all possible waves at once.
	// And the math to calulate mixed colors only supports 2 overlapping waves. So.
	const u16 overlapping_palette_size = 2;
	u16 overlapping_palette_banks[overlapping_palette_size];
	u16 overlapping_palette_indxs[overlapping_palette_size];

	SCR_ENTRY *se8 = &se_mem[SBB_8][0];

	// For each tile...
	for (u16 tile_y = wbounds.min_tile_y; tile_y <= wbounds.max_tile_y; ++tile_y)
	{
		for (u16 tile_x = wbounds.min_tile_x; tile_x <= wbounds.max_tile_x; ++tile_x)
		{
			// Set tile origin.
			u16 x_pos = 4 + tile_x * 8;
			u16 y_pos = 4 + tile_y * 8;

			// For each active wave...
			u16 wave_count = 0;
			for (u16 wave_i = 0; wave_i < wavefront_size; ++wave_i)
			{				
				if (wavefronts[wave_i].palette_bank > 0)
				{
					// ... If tile is close enough to wave, get palette info.
					u16 palette_index = get_overlapping_wave_details(x_pos, y_pos, wavefronts[wave_i]);
					
					if (palette_index > 0)
					{
						overlapping_palette_banks[wave_count] = wavefronts[wave_i].palette_bank; 
						overlapping_palette_indxs[wave_count] = palette_index;
						++wave_count;
					}

					// No point getting more than 2 overlaps, since calculations will ignore more than that.
					if (wave_count == 2)
					{
						break;
					}
				}
			}

			// Create SE entry.
			SCR_ENTRY se_entry = 0x0000;
			u16 se_index = tile_x + tile_y * screen_tile_pitch;
			
			if (wave_count == 1)
				se_entry = SE_PALBANK(overlapping_palette_banks[0]) | overlapping_palette_indxs[0];
			else if (wave_count == 2)
				se_entry = get_attenuated_palette_index(overlapping_palette_banks, overlapping_palette_indxs, overlapping_palette_size);
			
			// Update tilemap.
			se8[se_index] = se_entry;
		}
	}
}


// Advance all control deltas on wave.
void advance_wave(struct WAVEFRONT *wf)
{
	advance_control_delta(&(wf->radius));
	//advance_control_delta(&(wf->intensity));
}


// For each active wave, advance it. Deactivate it at arbitrary condition.
void update_waves(struct WAVEFRONT *wavefronts, u16 wavefront_size)
{
	// Find active wavefront entries.
	for (u16 i = 0; i < wavefront_size; ++i)
	{
		if (wavefronts[i].palette_bank > 0)
		{
			advance_wave(&(wavefronts[i]));
			
			// Deactivate wave once it reaches certain radius.
			if (wavefronts[i].radius.amount > 120)
			{
				wavefronts[i].palette_bank = 0;
			}
		}
	}
}


// Activate a wave in the object pool.
bool add_wave(struct WAVEFRONT *wavefronts, u16 wavefront_size, struct WAVEFRONT wavefront_to_add)
{
	// Find empty wavefront entry.
	for (u16 i = 0; i < wavefront_size; ++i)
	{
		if (wavefronts[i].palette_bank == 0)
		{
			wavefronts[i] = wavefront_to_add;
			return true;
		}
	}

	return false;
}


// Deactivate all waves in object pool.
void clear_waves(struct WAVEFRONT *wavefronts, u16 wavefront_size)
{
	for (u16 i = 0; i < wavefront_size; ++i)
	{
		wavefronts[i].palette_bank = 0;
	}
}


// Assign min and max tile indices for the wave window boundary.
// Waves will not be visible outside this window.
void set_wave_window_boundary(struct WTILEBOUNDS *wbounds)
{
	// Visible tiles on screen at once is (SCR_WT = 30, SCR_HT = 20)
	// Create window that is 1 tile away from screen edge on all sides.
	// Values in wbounds are 0-indexed!
	wbounds->min_tile_x = 1;
	wbounds->max_tile_x = SCR_WT - 2;
	wbounds->min_tile_y = 1;
	wbounds->max_tile_y = SCR_HT - 2;
}


// Construct border around area that waves visually appear within.
void build_wave_window_tilemap(struct WTILEBOUNDS *wbounds)
{
	SCR_ENTRY *se9 = &se_mem[SBB_9][0];
	
	// Tile ids used in border tilemap.
	const u16 h_edge_id = 0x10;
	const u16 v_edge_id = 0x11;
	const u16 corner_id = 0x12;
	
	// For each tile...
	for (u16 tile_y = wbounds->min_tile_y; tile_y <= wbounds->max_tile_y; ++tile_y)
	{
		// Only care if the tile is on the edge of the area.
		bool is_horizontal_edge = (tile_y == wbounds->min_tile_y || tile_y == wbounds->max_tile_y);

		for (u16 tile_x = wbounds->min_tile_x; tile_x <= wbounds->max_tile_x; ++tile_x)
		{
			// Only care if the tile is on the edge of the area.
			bool is_vertical_edge = (tile_x == wbounds->min_tile_x || tile_x == wbounds->max_tile_x);

			// Create tilemap entry. 
			// Place only when tile is on edge of area.
			SCR_ENTRY se_entry = 0x0000;

			// Flip tile if on far edge of area (possible double flip for corner).
			u16 flip = 0;
			if (tile_y == wbounds->max_tile_y)	flip |= SE_VFLIP;
			if (tile_x == wbounds->max_tile_x)	flip |= SE_HFLIP;

			// Build the full tilemap entry.
			if (is_horizontal_edge && is_vertical_edge)	se_entry = flip | corner_id;
			else if (is_horizontal_edge)				se_entry = flip | h_edge_id;
			else if (is_vertical_edge)					se_entry = flip | v_edge_id;

			// Assign (Note: 32 tiles across full screen, but last 2 are not visible without scrolling).
			se9[tile_x + tile_y * 32] = se_entry;
		}
	}
}


// Create a wavefront within the window bounds.
struct WAVEFRONT generate_wave(struct WTILEBOUNDS wbounds, u16 palette_bank, u16 radius_delta)
{	
	// Get position.
	u16 x = wbounds.min_tile_x + rand() % (wbounds.max_tile_x - wbounds.min_tile_x);
	u16 y = wbounds.min_tile_y + rand() % (wbounds.max_tile_y - wbounds.min_tile_y);
	x = 4 + x * 8; // Convert to pixel coordinates.
	y = 4 + y * 8; // Add 4 to offset to center of tile.
	struct ORIGIN origin = {.x = x, .y = y};
	
	// Set expansion rate.			
	struct CONTROL_DELTA radius = {.amount = 6, .delta = radius_delta, .delay = 1, .timer = 0};
	
	// Create wave.
	struct WAVEFRONT wf = {.palette_bank = palette_bank, .origin = origin, .radius = radius};
	
	return wf;
}


// Create a wavefront with a given origin.
struct WAVEFRONT generate_wave_xy(u16 x, u16 y, u16 palette_bank, u16 radius_delta)
{	
	// Set position.
	struct ORIGIN origin = {.x = x, .y = y};
	
	// Set expansion rate.			
	struct CONTROL_DELTA radius = {.amount = 6, .delta = radius_delta, .delay = 1, .timer = 0};
	
	// Create wave.
	struct WAVEFRONT wf = {.palette_bank = palette_bank, .origin = origin, .radius = radius};
	
	return wf;
}


int main()
{
	// VISUAL SETUP

	// ... Initilaize backgrounds.
	REG_BG0CNT = BG_CBB(CBB_0) | BG_SBB(SBB_8) | BG_4BPP | BG_PRIO(1);
	REG_BG1CNT = BG_CBB(CBB_0) | BG_SBB(SBB_9) | BG_4BPP | BG_PRIO(0);
	
	// ... Eenable tiled backgrounds.
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_BG1;
	
	// ... Create tiles and colors.
	load_tiles();
	load_colors();


	// SOUND SETUP
	// Volume is controlled by different registers: REG_SNDDMGCNT & REG_SNDxCNT

	// ... Enable sound.
	REG_SNDSTAT = BIT(7);

	// ... Set square wave mode 1 for speaker L and square wave mode 2 for speaker R.
	// ... Max volume for both speakers; considered the master volume control.
	// ... But, even setting this to 0 does not stop sound from the square wave mode (see below).
	REG_SNDDMGCNT =	SDMG_BUILD(SDMG_SQR1, SDMG_SQR2, 1, 1);

	// ... Disable sweep on DMG 1. Other channel doesn't have this option.
	REG_SND1SWEEP = 0;

	// ... Set envelope for square wave mode.
	// ... SSQR_ENV_BUILD(volume, increase/decrease over time, how fast to change; lower = faster)
	// ... Only setting this volume to 0 will stop the sound; or using the time mode.
	REG_SND1CNT = SSQR_ENV_BUILD(15, 0, 7) | SSQR_DUTY1_2;
	REG_SND2CNT = SSQR_ENV_BUILD(15, 0, 7) | SSQR_DUTY1_2;

	// ... Initialize frequency output in both speakers to 0/nothing.
	REG_SND1FREQ = 0;
	REG_SND2FREQ = 0;

	
	// OTHER SETUP.
	
	// Initialize wavefront object pool.
	const u16 wavefront_size = 10;
	struct WAVEFRONT wavefronts[wavefront_size];
	for (u16 i = 0; i < wavefront_size; ++i)
		wavefronts[i].palette_bank = 0;
	
	// Create window border around zone wavefronts stay within.
	struct WTILEBOUNDS wbounds;
	set_wave_window_boundary(&wbounds);
	build_wave_window_tilemap(&wbounds);

	// Initialize helper data for wavefront generation.
	u16 color_cycles[2] = {BANK_R, BANK_B};
	u16 sound_cycles[7] = {NOTE_C, NOTE_E, NOTE_G, NOTE_D, NOTE_F, NOTE_A, NOTE_B};

	u16 delta_volume = 1;
	u16 volume = 15;

	u16 slow_cycle = 1;
	u16 fast_cycle = 0;
	u16 fast_offset = 0;
	u16 last_fast_note = -1;

	u16 abberration_timer = 0;
	u16 abberration_duration = 8;
	u16 frames = 0;


	bool is_overlap_test_enabled = false;

	// MAIN LOOP.
	while(1)
	{
		vid_vsync();
		

		// Toggle test mode.
		{
			key_poll();

			bool reset = false;

			// Enable (L) or disable (R) the overlap test mode.
			if (key_hit(KEY_L)) 
				reset = is_overlap_test_enabled != true;
			else if (key_hit(KEY_R))
				reset = is_overlap_test_enabled != false;

			// Reset values when changing test mode.
			if (reset)
			{
				is_overlap_test_enabled = !is_overlap_test_enabled;
					
				// Reset other things.
				slow_cycle = 1;
				fast_cycle = 0;
				abberration_timer = 0;
				GREEN_SWAP = 0;
				frames = 0;

				// Flip volume direction.
				delta_volume *= -1;

				// Remove current waves.
				clear_waves(wavefronts, wavefront_size);
			}
		}

		// Disable chromatic abberration if on.
		// Short-circuiting to only increase timer when green swap is active.
		if (GREEN_SWAP && ++abberration_timer == abberration_duration)
		{
			GREEN_SWAP = abberration_timer = 0;
		}

		// Adjust volume between tests.
		if ((frames & 15) == 0)
		{
			volume = clamp(volume + delta_volume, 0, 7);
			REG_SND1CNT = SSQR_ENV_BUILD(volume, 0, 7) | SSQR_DUTY1_2;
			REG_SND2CNT = SSQR_ENV_BUILD(volume, 0, 7) | SSQR_DUTY1_2;
		}
		
		// During overlap test, generate 2 identical waves with different colors at the same location.
		if (is_overlap_test_enabled)
		{
			if ((frames & 15) == 0)
			{		
				for (u16 i = 0; i < 2; ++i)
				{
					slow_cycle = (slow_cycle + 1) % 3;
					u16 radius_delta = 2;
					u16 palette_bank = color_cycles[slow_cycle];
					
					struct WAVEFRONT wf = generate_wave_xy(120, 80, palette_bank, radius_delta);
					add_wave(wavefronts, wavefront_size, wf);
				}
			}
		}

		// During regular demo, generate a slow-moving wave + a fast-moving wave twice as often as the slow wave.
		else
		{	
			// Slow cycle.
			if ((frames & 63) == 0)
			{
				u16 radius_delta = 2;
				u16 palette_bank = BANK_G; // Always green for the bass note.
				GREEN_SWAP = 1;
				
				// Create wave.
				struct WAVEFRONT wf = generate_wave(wbounds, palette_bank, radius_delta);
				add_wave(wavefronts, wavefront_size, wf);
				
				// Play bass notes in C chord.
				u16 note = sound_cycles[slow_cycle];
				slow_cycle = (slow_cycle + 1) % 3;
				REG_SND1FREQ = SFREQ_RESET | SND_RATE(note, 0) | SFREQ_TIMED;
			}
			

			// Fast cycle.
			if ((frames & (15 + fast_offset)) == 0)
			{
				// Dynamically adjust how fast this cycle/wave plays again.
				u16 adjustment = rand() % 3; // 0, 1, 2
				fast_offset = (8 * adjustment) - 8;
				
				// The faster it will come back, the faster the radius delta will be.
				// This avoids more than 2 waves on the field at once. We don't like that yet.
				u16 radius_delta = 12 - (adjustment * 4);
				
				u16 palette_bank = color_cycles[fast_cycle];
				fast_cycle ^= 1;
				
				// Create wave.
				struct WAVEFRONT wf = generate_wave(wbounds, palette_bank, radius_delta);
				add_wave(wavefronts, wavefront_size, wf);
				
				// Play any note in the C scale. Higher octave than slow cycle.
				// Repeated notes go up an extra octave.
				u16 note = sound_cycles[rand() % 7];
				u16 octave = 1 + (note == last_fast_note ? 1 : 0);
				last_fast_note = note;
				REG_SND2FREQ = SFREQ_RESET | SND_RATE(note, octave) | SFREQ_TIMED;
			}
		}

		// Expand waves and color tiles.
		update_waves(wavefronts, wavefront_size);
		update_tiles(wavefronts, wavefront_size, wbounds);
		

		// Advance frames.
		++frames;
	}
	
	return 0;
}
