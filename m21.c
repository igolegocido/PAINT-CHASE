//How to play the game: move WASD to control Player 1 (blue block) and arrow keys to control Player 2 (red block)
//Right-side timer bar decrements as the game progresses. When it hits 0, the game ends
//The player whose colour fills more of the game board wins. That player's colour covers the screen (screen is white in case of a tie)
//KEY0 restarts the game. KEY1 pauses the game
//BUGS: graphics do not use double-buffering. unpause button does not erase correctly. player blocks have no collision detection when hitting each other
//TODO: separate player block from trail it leaves with a car sprite. warning when time is nearly up. audio cues. text on game over / start screens. making a start screen

//Definitions
#include <stdbool.h>
#include<stdlib.h>
#define TIMER_BASE 0xFF202000
#define PS2_BASE 0xFF200100
#define KEY_BASE 0xFF200050
const int SCREEN_HEIGHT = 240;
const int SCREEN_WIDTH = 320;
const int BLOCK_SIDELENGTH = 20;
const int MAX_TICKS = 20;


//Headers and global variables for the drawing functions. Does not use buffers nor wait to sync
void clear_screen(int colour);
void plot_pixel(int x, int y, short int line_color);
void draw();
void draw_rectangle(int x, int width, int y, int height, int colour);
void swap(int *a, int *b);
int pixel_buffer_start;
short int Buffer1[240][512]; // 240 rows, 512 (320 + padding) columns
short int Buffer2[240][512];
volatile int * pixel_ctrl_ptr = (int *)0xFF203020;
int blue = 0x2C3B; //rgb565 color picker: https://barth-dev.de/online/rgb565-color-picker/
int red = 0xF1C8;
int white = 0xFFFF;
void draw_countdown();
void draw_paused(bool on);
void wait_for_vsync();

//Headers and global variables for the FSM determining which screen to show
int grid[15][12];
int grid_xsize = 15;
int grid_ysize = 12;
int grid_content[180] = {0}; //at grid length of 20, (16 - 1)x12 grid
int grid_size = 180; //sizeof(array) is annoying.
int game_ticks = 0; //number of timer ticks recorded. when this exceeds MAX_TICKS, end the game
void game_over();
bool restart = true;
bool paused = false;
void start_game();
bool gameover_state = false;

//Headers and global variables that determine object directions
struct player{
	int x_coord;
	int y_coord;
	int dir_x;
	int dir_y;
	int dir;
	int col;
	int last_x;
	int last_y;
};
void increment(struct player *p);
struct player player1 = {0, 0, 0, 0, -1, 0x2C3B, 0, 0}; //start top left
struct player player2 = {SCREEN_WIDTH - 2*BLOCK_SIDELENGTH, SCREEN_HEIGHT - BLOCK_SIDELENGTH, 0, 0, -1, 0xF1C8, 0, 0}; //start bottom right

//Headers and global variables for interrupts
static void handler(void) __attribute__ ((interrupt ("machine")));
//For PS2 keyboard
void set_PS2();
volatile int *PS2_ptr = (int*) 0xFF200100;
void keyboard_ISR();
//For interval timer
void set_itimer();
volatile int *timer_ptr = (int *) TIMER_BASE;
void itimer_ISR();
int clock_time = 100000000; //100MZ clock
//For keys
volatile int *KEY_ptr = (int *) KEY_BASE;
void set_KEYS();
void KEY_ISR();

int main(void)
{
    /* Read location of the pixel buffer from the pixel buffer controller */
    pixel_buffer_start = *pixel_ctrl_ptr;
	start_game();
	
	//Setup interrupts
	//Set up PS2 keyboard to allow interrupts
	set_PS2();
	//Set up interval timer
	set_itimer();
	//Set up KEY0 = restart game
	set_KEYS();
	//Set up double buffer
	//set_BUFFER();
	
	
	//Set up interrupts from interrupts side
	int mstatus_value = 0b1000; //interrupt bit mask
	// disable interrupts
	__asm__ volatile ("csrc mstatus, %0" :: "r"(mstatus_value));
	int mtvec_value = (int) &handler; // set trap address
	__asm__ volatile ("csrw mtvec, %0" :: "r"(mtvec_value));
	// disable all interrupts that are currently enabled
	int mie_value;
	__asm__ volatile ("csrr %0, mie" : "=r"(mie_value));
	__asm__ volatile ("csrc mie, %0" :: "r"(mie_value));
	mie_value = 0x450000; // set value to allow external interrupts (ps2 keyboard = 0x400000, itimer = 0x10000, keys = 0x40000)
	// set interrupt enables
	__asm__ volatile ("csrs mie, %0" :: "r"(mie_value));
	// enable Nios V interrupts
	__asm__ volatile ("csrs mstatus, %0" :: "r"(mstatus_value));
	
	while(1){ //endless loop
		//Case to bring on the start of the game. During first loop and if player presses KEY0
		if(restart)
			start_game();
		else if(paused) //Case to pause the game. Happens when player presses KEY1
			while(paused){
				draw_paused(true);
			}
		else if(game_ticks >= MAX_TICKS) //Case when game is over
			game_over();
		else //Case if nothing else is triggered; keep playing the game
			draw();
	}
}

//************************************************** Setup functions **************************************************

//Configure the FPGA interval timer
void set_itimer(){
	int load_val = clock_time / 1.5; //This determines speed of the blocks' movement
	*(timer_ptr + 0x2) = (load_val & 0xFFFF);
	*(timer_ptr + 0x3) = (load_val >> 16) & 0xFFFF;
	// start interval timer, enable its interrupts
	*(timer_ptr + 1) = 0x7; // STOP = 1, START = 1, CONT = 1, ITO = 1
}

//Configure the PS2 keyboard
void set_PS2(){
	*(PS2_ptr + 1) = 0x00000001; //set RE in PS2_Control to 1 to allow interrupts
}

//Configure KEY0
void set_KEYS(){
	*(KEY_ptr + 3) = 0xF; // clear EdgeCapture register
	*(KEY_ptr + 2) = 0x3; // enable interrupts for KEY0, KEY1
}

//************************************************** Interrupt functions **************************************************

//Interrupt handler: check which device triggered the interrupt
void handler(){
	int mcause_value;
	__asm__ volatile ("csrr %0, mcause" : "=r"(mcause_value));
	mcause_value = mcause_value & 0x7FFFFFFF; //get rid of bit 31
	if(mcause_value == 22){ //irq for keyboard
		keyboard_ISR();
	}else if(mcause_value == 16){ //irq for interval timer
		itimer_ISR();
	}else if(mcause_value == 18){ //irq for keys
		KEY_ISR();
	} //otherwise ignore
}

//Functionality when keyboard interrupt is called
void keyboard_ISR(){
	int PS2_data = *(PS2_ptr);
	char key_input = 0;

	int RVALID = PS2_data & 0x8000;
	if(RVALID){
		key_input = PS2_data & 0xFF; //Get value in the data
		
		//Determine direction based on pressed key
		//For Player 1
		if(key_input == 0x23) //'D' key
			player1.dir = 1;
		else if(key_input == 0x1B) //'S' key
			player1.dir = 2;
		else if(key_input == 0x1C) //'A' key
			player1.dir = 3;
		else if(key_input == 0x1D) //'W' key
			player1.dir = 4;
		
		//For Player 2
		if(key_input == 0x74) //Right arrow key
			player2.dir = 1;
		else if(key_input == 0x72) //Down arrow key
			player2.dir = 2;
		else if(key_input == 0x6B) //Left arrow key
			player2.dir = 3;
		else if(key_input == 0x75) //Up arrow key
			player2.dir = 4;
	}
}

// FPGA interval timer interrupt service routine
void itimer_ISR(void){
	volatile int * timer_ptr = (int *) TIMER_BASE;
	*timer_ptr = 0; // clear the interrupt
	/*if(game_ticks <= MAX_TICKS){
		increment(&player1);
		increment(&player2);
	}*/
	
	//increase game_ticks
	if(game_ticks < MAX_TICKS && !paused){
		game_ticks++;
	}
}

//KEYs interrupt handling, KEY0 and KEY1 here
void KEY_ISR(void){
	int pressed;
	pressed = *(KEY_ptr + 3); // read EdgeCapture
	*(KEY_ptr + 3) = pressed; // clear EdgeCapture register
	if(pressed & 0b0001) //if KEY0 pressed, tell game to restart
		restart = true;
	if(pressed & 0b0010){ //if KEY1 pressed, flip game state from paused to unpaused and vice versa
		if(paused)
			draw_paused(false);
		paused = !paused;
	}
}

//************************************************** Game start functions **************************************************

void start_game(){
	clear_screen(0); //draw everything black
	
	//Draw initial countdown bar
	/*for(int i = SCREEN_WIDTH - BLOCK_SIDELENGTH; i < SCREEN_WIDTH; i++){
		for(int j = 0; j < SCREEN_HEIGHT; j++){
			plot_pixel(i, j, 0xFFFF);
		}
	}*/
	
	//Reset all necessary global variables to initial state
	for(int i = 0; i < grid_size; i++){
		grid_content[i] = 0;
	}
	game_ticks = 0;
	restart = false;
	paused = false;
	player1 = (struct player) {0, 0, 1, 0, -1, 0x2C3B, 0, 0};
	player2 = (struct player) {SCREEN_WIDTH - 2*BLOCK_SIDELENGTH, SCREEN_HEIGHT - BLOCK_SIDELENGTH, -1, 0, -1, 0xF1C8, 0, 0};
	
	for(int i = 0; i < grid_xsize; i ++){
		for(int j = 0; j < grid_ysize; j++){
			grid[i][j] = 0;
		}
	}
	draw_grid();
}

//************************************************** Object direction function **************************************************

//Increment is called every time interval timer is triggered
void increment(struct player *p){
	//Erase old boxes
	//draw_box(x, y, 0);
	
	//Directions: -1 = don't change, 1 = right, 2 = down, 3 = left, 4 = up
	int direction = p->dir;
	int dx = p->dir_x;
	int dy = p->dir_y;
	int x = p->x_coord;
	int y = p->y_coord;
	if(direction == 1){ //right
		dx = 1;
		dy = 0;
	}else if(direction == 2){ //down
		dx = 0;
		dy = 1;
	}else if(direction == 3){ //left
		dx = -1;
		dy = 0;
	}else if(direction == 4){ //up
		dx = 0;
		dy = -1;
	}
	
	//update last position of x and y;
	p->last_x = x;
	p->last_y = y;
	
	//Add velocity to coordinates
	int new_x = x + dx;
	int new_y = y + dy;
	
	//Check that coordinates are not out of bound, set velocity to 0 if they are
	if(new_x <= 0){
		new_x = 0;
		dx = 0;
	}else if(new_x >= SCREEN_WIDTH - 2*BLOCK_SIDELENGTH){
		new_x = SCREEN_WIDTH - 2*BLOCK_SIDELENGTH;
		dx = 0;
	}
	if(new_y <= 0){
		new_y = 0;
		dy = 0;
	}else if(new_y >= SCREEN_HEIGHT - BLOCK_SIDELENGTH){
		new_y = SCREEN_HEIGHT - BLOCK_SIDELENGTH;
		dy = 0;
	}
	//Check that the two boxes are not going to hit each other
	//todo
	
	//Check that the two boxes are not going to hit each other
	if(p->col == player1.col){
		if(new_x + BLOCK_SIDELENGTH >= player2.x_coord && new_x <= player2.x_coord
		   && new_y + BLOCK_SIDELENGTH >= player2.y_coord && new_y <= player2.y_coord){
			//clear_screen(blue);
			new_x = x;
			new_y = y;
			dx = 0;
			dy = 0;
		}
			
	}else{
		if(new_x + BLOCK_SIDELENGTH >= player1.x_coord && new_x <= player1.x_coord
		   && new_y + BLOCK_SIDELENGTH >= player1.y_coord && new_y <= player1.y_coord){
			//clear_screen(red);
			new_x = x;
			new_y = y;
			dx = 0;
			dy = 0;
		}
	}
	
	
	int grid_x = (new_x+10)/BLOCK_SIDELENGTH;
	int grid_y = (new_y+10)/BLOCK_SIDELENGTH;;
	grid[grid_x][grid_y] = p->col;
	draw_singleGrid(grid_x, grid_y);
	if(grid_x != 0 && grid_x != grid_xsize)
	grid_x -= dx;
	if(grid_y != 0 && grid_y != grid_ysize)
	grid_y -= dy;
	draw_singleGrid(grid_x, grid_y);
	
	//Apply new coordinates
	p->x_coord = new_x;
	p->y_coord = new_y;
	p->dir_x = dx;
	p->dir_y = dy;
	
	//Add score to the grid
	int num_grids_width = SCREEN_WIDTH/BLOCK_SIDELENGTH - 1;
	int grid_index = new_x/BLOCK_SIDELENGTH + new_y/BLOCK_SIDELENGTH*num_grids_width; //calculate index given coordinates
	if(p->col == blue) //set colour in grid array to track what's on the screen
		grid_content[grid_index] = blue;
	else
		grid_content[grid_index] = red;
}

//************************************************** Drawing functions **************************************************
//(Feel free to replace with your own)

void clear_screen(int colour){
	int y, x;
	for (x = 0; x < SCREEN_WIDTH; x++)
		for (y = 0; y < SCREEN_HEIGHT; y++)
			plot_pixel(x, y, colour);
}

void draw(){
	//remove past draw
	//draw_rectangle(player1.last_x+4, BLOCK_SIDELENGTH-10, player1.last_y+4, BLOCK_SIDELENGTH-10, 0);
	//draw_rectangle(player2.last_x+4, BLOCK_SIDELENGTH-10, player2.last_y+4, BLOCK_SIDELENGTH-10, 0);
	increment(&player1);
	increment(&player2);
	//draw player
	draw_rectangle(player1.x_coord+4, BLOCK_SIDELENGTH-10, player1.y_coord+4, BLOCK_SIDELENGTH-10, player2.col);
	draw_rectangle(player2.x_coord+4, BLOCK_SIDELENGTH-10, player2.y_coord+4, BLOCK_SIDELENGTH-10, player1.col);
	draw_countdown();
	
	wait_for_vsync(); // swap front and back buffers on VGA vertical sync
    pixel_buffer_start = *(pixel_ctrl_ptr + 1); // new back buffer
}
void draw_grid(){
	for(int i = 0; i < grid_xsize; i ++){
		for(int j = 0; j < grid_ysize; j++){
			draw_singleGrid(i,j);
		}
	}
}

void draw_singleGrid(int x, int y){
	draw_rectangle(x*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, y*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, white);
	draw_rectangle(x*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH-2, y*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH-2, grid[x][y]);
}
void swap(int *a, int *b){
	int temp = *a;
	*a = *b;
	*b = temp;
}

void draw_rectangle(int x, int width, int y, int height, int colour){
	for(int i = 0; i < width; i++){
		for(int j = 0; j < height; j++){
			plot_pixel(x + i, y + j, colour);
		}
	}
}

void plot_pixel(int x, int y, short int line_color)
{
    volatile short int *one_pixel_address;
    one_pixel_address = pixel_buffer_start + (y << 10) + (x << 1);
    *one_pixel_address = line_color;
}

//This draws the countdown bar in the right
void draw_countdown(){
	//White bar is drawn at start of game, this draws a black rectangle to reduce its size
	int height = SCREEN_HEIGHT / MAX_TICKS; //this is height of each tick
	draw_rectangle(SCREEN_WIDTH - BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, 0, height*game_ticks, 0);
	draw_rectangle(SCREEN_WIDTH - BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, height*game_ticks, SCREEN_HEIGHT-height*game_ticks, white);
	
	
	//i is y-coord, j is x-coord, because no double-buffer so i want to draw lines horizontally
	/*for(int i = 0; i < height*game_ticks; i++){
		for(int j = SCREEN_WIDTH - BLOCK_SIDELENGTH; j < SCREEN_WIDTH; j++){
			plot_pixel(j, i, 0);
		}
	}*/
}

//This draws the paused symbol and erases it when unpaused
void draw_paused(bool on){
	int draw_col = 0;
	if(on)
		draw_col = white;
	int bar_height = BLOCK_SIDELENGTH * 6;
	int start_x = (SCREEN_WIDTH - 4 * BLOCK_SIDELENGTH)/2;
	int start_y = (SCREEN_HEIGHT - bar_height)/2;
	
	if(on){
		draw_rectangle(start_x, BLOCK_SIDELENGTH, start_y, bar_height, draw_col);
		draw_rectangle(start_x + 3 * BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, start_y, bar_height, draw_col);
	}else{ //this is to replace the pause button with the screen underneath upon unpausing it
		/*int index_x = start_x/BLOCK_SIDELENGTH;
		int index_y = start_y/BLOCK_SIDELENGTH;
		int grid_width = SCREEN_WIDTH/BLOCK_SIDELENGTH - 1;
		for(int i = 0; i < 6; i++){
			draw_col = grid_content[index_x + (index_y + i)*grid_width];
			draw_rectangle(start_x, BLOCK_SIDELENGTH, start_y + i*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, draw_col);
			draw_col = grid_content[index_x + 3 + (index_y + i)*grid_width];
			draw_rectangle(start_x + 3 * BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, start_y + i*BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, draw_col);
		}*/
		//that function doesn't work so currently it just draws black
		draw_rectangle(start_x, BLOCK_SIDELENGTH, start_y, bar_height, draw_col);
		draw_rectangle(start_x + 3 * BLOCK_SIDELENGTH, BLOCK_SIDELENGTH, start_y, bar_height, draw_col);
	}
	wait_for_vsync(); // swap front and back buffers on VGA vertical sync
    pixel_buffer_start = *(pixel_ctrl_ptr + 1); // new back buffer
	
}

//************************************************** Game over functions **************************************************
//Changes screen to the winning player's colour. White if they are tied
void game_over(){
	int blue_score = 0;
	int red_score = 0;
	
	//Calculate number of tiles of each colour in the grid array
	for(int i = 0; i < grid_size; i++){
		if(grid_content[i] == blue)
			blue_score++;
		else if(grid_content[i] == red)
			red_score++;
	}
	
	//Clear screen based on winning colour
	if(blue_score > red_score)
		clear_screen(blue);
	else if(red_score > blue_score)
		clear_screen(red);
	else
		clear_screen(white);
}

void wait_for_vsync()
{
	volatile int * pixel_ctrl_ptr = (int *) 0xff203020; // base address
	int status;
	*pixel_ctrl_ptr = 1; // start the synchronization process
	// - write 1 into front buffer address register
	status = *(pixel_ctrl_ptr + 3); // read the status register
	while ((status & 0x01) != 0) // polling loop waiting for S bit to go to 0
		{
		status = *(pixel_ctrl_ptr + 3);
	}
}
void set_BUFFER(){
	
	 /* set front pixel buffer to Buffer 1 */
    *(pixel_ctrl_ptr + 1) = (int) &Buffer1; // first store the address in the  back buffer
    /* now, swap the front/back buffers, to set the front buffer location */
    wait_for_vsync();
    /* initialize a pointer to the pixel buffer, used by drawing functions */
    pixel_buffer_start = *pixel_ctrl_ptr;
    clear_screen(0); // pixel_buffer_start points to the pixel buffer

    /* set back pixel buffer to Buffer 2 */
    *(pixel_ctrl_ptr + 1) = (int) &Buffer2;
    pixel_buffer_start = *(pixel_ctrl_ptr + 1); // we draw on the back buffer
    clear_screen(0); // pixel_buffer_start points to the pixel buffer
}