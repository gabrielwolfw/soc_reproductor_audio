#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Audio Player with Simple Tone Generator and MM:SS Display");

// Virtual address base for lightweight bridge
void *lwbridgebase;

// Audio player state
typedef enum {
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_STOPPED
} audio_state_t;

static audio_state_t current_state = AUDIO_STOPPED;
static int current_track = 0;
static const int total_tracks = 3;

// Time tracking (in seconds)
static int current_time_seconds = 0;
static int current_time_minutes = 0;
static struct timer_list audio_timer;
static struct timer_list display_timer;

// Button definitions
#define BUTTON_PLAY_PAUSE   0x1  // Button 0
#define BUTTON_NEXT         0x2  // Button 1  
#define BUTTON_PREV         0x4  // Button 2

// Register offsets
#define BUTTONS_BASE_OFFSET     0x8800
#define BUTTONS_DATA_OFFSET     0x0
#define BUTTONS_INTERRUPT_MASK  0x8
#define BUTTONS_EDGE_CAPTURE    0xC

#define AUDIO_BASE_OFFSET       0x8860
#define AUDIO_FIFOSPACE_REG     0x4
#define AUDIO_LEFTDATA_REG      0x8
#define AUDIO_RIGHTDATA_REG     0xC
#define AUDIO_CONTROL_REG       0x0

#define SEVEN_SEGMENTS_BASE_OFFSET 0x8810

// Simple tone generation (based on your working code)
static int tone_frequencies[] = {440, 523, 659}; // A4, C5, E5
static unsigned int tone_phase = 0;
static unsigned int sample_rate = 48000;

// Simple sine wave table (precomputed for kernel space)
static const int sine_table[256] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767, 32757, 32728, 32678, 32609, 32521, 32412, 32285, 32137, 31971, 31785, 31580, 31356, 31113, 30852, 30571,
    30273, 29956, 29621, 29268, 28898, 28510, 28105, 27683, 27245, 26790, 26319, 25832, 25329, 24811, 24279, 23731,
    23170, 22594, 22005, 21403, 20787, 20159, 19519, 18868, 18204, 17530, 16846, 16151, 15446, 14732, 14010, 13279,
    12539, 11793, 11039, 10278, 9512, 8739, 7962, 7179, 6393, 5602, 4808, 4011, 3212, 2410, 1608, 804,
    0, -804, -1608, -2410, -3212, -4011, -4808, -5602, -6393, -7179, -7962, -8739, -9512, -10278, -11039, -11793,
    -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
    -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790, -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
    -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
    -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
    -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683, -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
    -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
    -12539, -11793, -11039, -10278, -9512, -8739, -7962, -7179, -6393, -5602, -4808, -4011, -3212, -2410, -1608, -804
};

// Debouncing
static struct timespec last_button_time[3];
#define DEBOUNCE_TIME_MS 200

// Function declarations
void audio_play(void);
void audio_pause(void);
void audio_next_track(void);
void audio_prev_track(void);
void display_time_mmss(void);
void generate_audio_samples(void);
void audio_timer_callback(unsigned long data);
void display_timer_callback(unsigned long data);
int is_button_debounced(int button_num);

// 7-segment patterns (inverted for common cathode)
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,  // 0,1,2,3,4
    0x12, 0x02, 0x78, 0x00, 0x10   // 5,6,7,8,9
};

// Display MM:SS format
void display_time_mmss(void)
{
    uint32_t display_value = 0;
    uint8_t min_tens = (current_time_minutes / 10) % 10;
    uint8_t min_ones = current_time_minutes % 10;
    uint8_t sec_tens = (current_time_seconds / 10) % 10;
    uint8_t sec_ones = current_time_seconds % 10;
    
    // Pack into 32-bit value for 4 displays (7 bits each)
    // HEX3(bits 27-21) | HEX2(bits 20-14) | HEX1(bits 13-7) | HEX0(bits 6-0)
    display_value = ((uint32_t)seven_seg_patterns[min_tens] << 21) |
                   ((uint32_t)seven_seg_patterns[min_ones] << 14) |
                   ((uint32_t)seven_seg_patterns[sec_tens] << 7) |
                   ((uint32_t)seven_seg_patterns[sec_ones]);
    
    iowrite32(display_value, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    
    printk(KERN_INFO "Time: %02d:%02d (Track %d)\n", 
           current_time_minutes, current_time_seconds, current_track);
}

// Generate audio samples (based on your working method)
void generate_audio_samples(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right;
    int i;
    
    if (current_state != AUDIO_PLAYING) return;
    
    // Check FIFO space (mimicking your working code)
    fifospace = ioread32(lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_FIFOSPACE_REG);
    write_space_left = (fifospace >> 24) & 0xFF;   // WSLC
    write_space_right = (fifospace >> 16) & 0xFF;  // WSRC
    
    // Generate multiple samples if FIFO has space
    for (i = 0; i < 10 && write_space_left > 0 && write_space_right > 0; i++) {
        // Calculate phase index for sine table
        unsigned int phase_increment = (tone_frequencies[current_track] * 256) / sample_rate;
        unsigned int table_index = (tone_phase >> 8) & 0xFF;
        
        // Get sine wave sample and amplify (like your working code)
        int sample = sine_table[table_index];
        sample = sample / 2; // Reduce volume slightly
        
        // Convert to 16-bit and create 32-bit sample
        int16_t sample_16 = (int16_t)sample;
        uint32_t sample_32 = ((uint32_t)(sample_16 & 0xFFFF) << 16) | (sample_16 & 0xFFFF);
        
        // Write to both channels (like your working code)
        iowrite32(sample_32, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_LEFTDATA_REG);
        iowrite32(sample_32, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_RIGHTDATA_REG);
        
        // Update phase
        tone_phase += phase_increment;
        if (tone_phase >= (256 * 256)) tone_phase = 0;
        
        write_space_left--;
        write_space_right--;
    }
}

// Audio timer callback (generates samples)
void audio_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        generate_audio_samples();
        // Reschedule timer for continuous audio (1ms like your working code)
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(1));
    }
}

// Display timer callback (updates time every second)
void display_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        current_time_seconds++;
        if (current_time_seconds >= 60) {
            current_time_seconds = 0;
            current_time_minutes++;
            if (current_time_minutes >= 100) {
                current_time_minutes = 0; // Reset after 99:59
            }
        }
        display_time_mmss();
    }
    
    // Reschedule for 1 second
    mod_timer(&display_timer, jiffies + HZ);
}

// Debouncing function
int is_button_debounced(int button_num)
{
    struct timespec current_time;
    long diff_ms;
    
    getnstimeofday(&current_time);
    
    diff_ms = (current_time.tv_sec - last_button_time[button_num].tv_sec) * 1000 +
              (current_time.tv_nsec - last_button_time[button_num].tv_nsec) / 1000000;
    
    if (diff_ms > DEBOUNCE_TIME_MS) {
        last_button_time[button_num] = current_time;
        return 1;
    }
    return 0;
}

// IRQ Handler
irq_handler_t button_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    uint32_t button_value;
    uint32_t edge_capture;
    
    edge_capture = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    button_value = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_DATA_OFFSET);
    
    printk(KERN_INFO "Button interrupt: edge_capture=0x%x, button_value=0x%x\n", 
           edge_capture, button_value);
    
    if ((edge_capture & BUTTON_PLAY_PAUSE) && is_button_debounced(0)) {
        printk(KERN_INFO "Play/Pause button pressed\n");
        if (current_state == AUDIO_PLAYING) {
            audio_pause();
        } else {
            audio_play();
        }
    }
    
    if ((edge_capture & BUTTON_NEXT) && is_button_debounced(1)) {
        printk(KERN_INFO "Next button pressed\n");
        audio_next_track();
    }
    
    if ((edge_capture & BUTTON_PREV) && is_button_debounced(2)) {
        printk(KERN_INFO "Previous button pressed\n");
        audio_prev_track();
    }
    
    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    return (irq_handler_t) IRQ_HANDLED;
}

// Audio control functions
void audio_play(void)
{
    printk(KERN_INFO "AUDIO: Starting playback - Track %d, Frequency %dHz\n", 
           current_track, tone_frequencies[current_track]);
    
    current_state = AUDIO_PLAYING;
    tone_phase = 0; // Reset phase
    
    // Initialize audio hardware (like your working code)
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG); // Reset
    mdelay(1);
    iowrite32(0x1, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG); // Enable
    mdelay(1);
    
    // Start timers
    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(1));
    mod_timer(&display_timer, jiffies + HZ);
    
    display_time_mmss();
}

void audio_pause(void)
{
    printk(KERN_INFO "AUDIO: Pausing playback\n");
    
    current_state = AUDIO_PAUSED;
    
    // Stop audio output
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    // Timers will stop themselves when state != PLAYING
}

void audio_next_track(void)
{
    int was_playing = (current_state == AUDIO_PLAYING);
    
    if (was_playing) {
        audio_pause();
    }
    
    current_track = (current_track + 1) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    printk(KERN_INFO "AUDIO: Next track -> %d (Frequency: %dHz)\n", 
           current_track, tone_frequencies[current_track]);
    
    display_time_mmss();
    
    if (was_playing) {
        audio_play();
    }
}

void audio_prev_track(void)
{
    int was_playing = (current_state == AUDIO_PLAYING);
    
    if (was_playing) {
        audio_pause();
    }
    
    current_track = (current_track - 1 + total_tracks) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    printk(KERN_INFO "AUDIO: Previous track -> %d (Frequency: %dHz)\n", 
           current_track, tone_frequencies[current_track]);
    
    display_time_mmss();
    
    if (was_playing) {
        audio_play();
    }
}

// Module initialization
static int __init initialize_audio_button_handler(void)
{
    int result;
    int i;
    struct timespec current_time;
    
    printk(KERN_INFO "=== Initializing Tone Generator Audio Player ===\n");
    
    // Initialize debounce timestamps
    getnstimeofday(&current_time);
    for (i = 0; i < 3; i++) {
        last_button_time[i] = current_time;
    }
    
    // Initialize timers
    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    audio_timer.data = 0;
    
    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    display_timer.data = 0;
    
    // Map memory
    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    if (!lwbridgebase) {
        printk(KERN_ERR "Failed to map lwbridge base address\n");
        return -ENOMEM;
    }
    
    printk(KERN_INFO "Memory mapped successfully\n");
    
    // Initialize displays and audio
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();
    
    // Initialize audio (like your working code)
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    // Setup interrupts
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler, 
                        IRQF_SHARED, "audio_button_irq", 
                        (void *)(button_irq_handler));
    
    if (result) {
        printk(KERN_ERR "Failed to register IRQ handler: %d\n", result);
        iounmap(lwbridgebase);
        return result;
    }
    
    printk(KERN_INFO "=== Audio Player initialized ===\n");
    printk(KERN_INFO "Track %d: %dHz tone\n", 
           current_track, tone_frequencies[current_track]);
    
    return 0;
}

// Module cleanup
static void __exit cleanup_audio_button_handler(void)
{
    printk(KERN_INFO "=== Cleaning up Audio Player ===\n");
    
    // Stop audio and timers
    current_state = AUDIO_STOPPED;
    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);
    
    // Stop audio hardware
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    // Cleanup interrupts
    iowrite32(0x0, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    free_irq(72 + 4, (void*)button_irq_handler);
    
    // Clear display
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    
    // Unmap memory
    if (lwbridgebase) {
        iounmap(lwbridgebase);
    }
    
    printk(KERN_INFO "=== Audio Player cleanup complete ===\n");
}

module_init(initialize_audio_button_handler);
module_exit(cleanup_audio_button_handler);