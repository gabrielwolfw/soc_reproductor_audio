#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kthread.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Chunked Audio Player with Background Loading");

// Virtual address base for lightweight bridge
void *lwbridgebase;

// Audio player state
typedef enum {
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_STOPPED
} audio_state_t;

static audio_state_t current_state = AUDIO_STOPPED;

// DUAL CHUNK SYSTEM
#define CHUNK_SIZE (32 * 1024)  // 32KB chunks
static uint8_t chunk_a[CHUNK_SIZE];
static uint8_t chunk_b[CHUNK_SIZE];
static uint8_t *active_chunk;
static uint8_t *loading_chunk;
static size_t active_chunk_size = 0;
static size_t chunk_pos = 0;
static int chunk_ready = 0;

// File management
static struct file *audio_file = NULL;
static loff_t file_pos = 44;  // Skip WAV header
static loff_t file_size = 0;
static int file_open = 0;

// Background loading thread
static struct task_struct *loader_thread = NULL;
static int loader_should_run = 0;
static int loading_in_progress = 0;

// Time tracking
static int current_time_seconds = 0;
static int current_time_minutes = 0;
static struct timer_list audio_timer;
static struct timer_list display_timer;

// Button definitions
#define BUTTON_PLAY_PAUSE   0x1
#define BUTTON_NEXT         0x2
#define BUTTON_PREV         0x4

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

// Sample processing
static int sample_counter = 0;
static int samples_per_call = 8;  // Optimized for 8kHz

// Debouncing
static unsigned long last_button_jiffies = 0;

// 7-segment patterns
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Function declarations
void audio_play_chunked(void);
void audio_pause_chunked(void);
void display_time_mmss(void);
void generate_audio_samples_chunked(void);
int open_audio_file_chunked(void);
int load_next_chunk(void);
int loader_thread_func(void *data);
void swap_chunks(void);
void audio_timer_callback(unsigned long data);
void display_timer_callback(unsigned long data);

// OPEN FILE AND GET SIZE
int open_audio_file_chunked(void)
{
    if (audio_file) {
        filp_close(audio_file, NULL);
        audio_file = NULL;
    }
    
    audio_file = filp_open("/media/sd/songs/song1.wav", O_RDONLY, 0);
    if (IS_ERR(audio_file)) {
        printk(KERN_ERR "Cannot open audio file: %ld\n", PTR_ERR(audio_file));
        audio_file = NULL;
        return 0;
    }
    
    // Get file size
    file_size = i_size_read(file_inode(audio_file));
    file_pos = 44;  // Skip WAV header
    file_open = 1;
    
    printk(KERN_INFO "Audio file opened: %lld bytes\n", file_size);
    return 1;
}

// LOAD CHUNK (called from background thread)
int load_next_chunk(void)
{
    mm_segment_t oldfs;
    int bytes_read;
    
    if (!file_open || !audio_file || loading_in_progress) {
        return 0;
    }
    
    loading_in_progress = 1;
    
    // Check if we've reached end of file
    if (file_pos >= file_size) {
        // Loop back to start
        file_pos = 44;
        printk(KERN_INFO "Looping audio file\n");
    }
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    bytes_read = vfs_read(audio_file, loading_chunk, CHUNK_SIZE, &file_pos);
    
    set_fs(oldfs);
    
    if (bytes_read > 0) {
        // Swap chunks atomically
        swap_chunks();
        active_chunk_size = bytes_read;
        chunk_pos = 0;
        chunk_ready = 1;
        
        printk(KERN_INFO "Chunk loaded: %d bytes (pos: %lld)\n", bytes_read, file_pos);
    } else {
        printk(KERN_WARNING "Failed to load chunk: %d\n", bytes_read);
    }
    
    loading_in_progress = 0;
    return bytes_read;
}

// ATOMIC CHUNK SWAP
void swap_chunks(void)
{
    uint8_t *temp = active_chunk;
    active_chunk = loading_chunk;
    loading_chunk = temp;
}

// BACKGROUND LOADER THREAD
int loader_thread_func(void *data)
{
    printk(KERN_INFO "Loader thread started\n");
    
    while (loader_should_run && !kthread_should_stop()) {
        // Check if we need to load next chunk
        if (chunk_ready && chunk_pos >= active_chunk_size - 1024) {
            // Time to load next chunk
            chunk_ready = 0;
            load_next_chunk();
        }
        
        // Sleep for a bit
        msleep(50);
    }
    
    printk(KERN_INFO "Loader thread stopped\n");
    return 0;
}

// Display time
void display_time_mmss(void)
{
    uint32_t display_value;
    uint8_t min_tens, min_ones, sec_tens, sec_ones;
    
    min_tens = (current_time_minutes / 10) % 10;
    min_ones = current_time_minutes % 10;
    sec_tens = (current_time_seconds / 10) % 10;
    sec_ones = current_time_seconds % 10;
    
    display_value = ((uint32_t)seven_seg_patterns[min_tens] << 21) |
                   ((uint32_t)seven_seg_patterns[min_ones] << 14) |
                   ((uint32_t)seven_seg_patterns[sec_tens] << 7) |
                   ((uint32_t)seven_seg_patterns[sec_ones]);
    
    iowrite32(display_value, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
}

// OPTIMIZED audio generation - only memory access
void generate_audio_samples_chunked(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right;
    int i;
    
    if (current_state != AUDIO_PLAYING || !chunk_ready) return;
    
    fifospace = ioread32(lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_FIFOSPACE_REG);
    write_space_left = (fifospace >> 24) & 0xFF;
    write_space_right = (fifospace >> 16) & 0xFF;
    
    // Process multiple samples for efficiency
    for (i = 0; i < samples_per_call && write_space_left > 0 && write_space_right > 0; i++) {
        int16_t sample = 0;
        uint32_t sample_32;
        
        // Light sample rate control for 8kHz
        sample_counter++;
        if (sample_counter % 2 != 0) continue;
        
        // Check chunk bounds
        if (chunk_pos + 1 >= active_chunk_size) {
            // Current chunk exhausted, wait for next
            return;
        }
        
        // Read 16-bit mono sample from active chunk
        if (chunk_pos + 1 < active_chunk_size) {
            sample = *(int16_t*)(&active_chunk[chunk_pos]);
            chunk_pos += 2;
        } else {
            sample = 0;
        }
        
        // Light volume control
        sample = sample / 3;
        
        // Mono to stereo
        sample_32 = ((uint32_t)(sample & 0xFFFF) << 16) | (sample & 0xFFFF);
        
        // Write to audio hardware efficiently
        iowrite32(sample_32, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_LEFTDATA_REG);
        iowrite32(sample_32, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_RIGHTDATA_REG);
        
        write_space_left--;
        write_space_right--;
    }
}

// Timer callbacks
void audio_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        generate_audio_samples_chunked();
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(15));  // 15ms for 8kHz
    }
}

void display_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        current_time_seconds++;
        if (current_time_seconds >= 60) {
            current_time_seconds = 0;
            current_time_minutes++;
            if (current_time_minutes >= 100) {
                current_time_minutes = 0;
            }
        }
        display_time_mmss();
    }
    mod_timer(&display_timer, jiffies + HZ);
}

// IRQ Handler
irq_handler_t button_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    uint32_t edge_capture;
    unsigned long current_jiffies = jiffies;
    
    edge_capture = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    
    if (time_after(current_jiffies, last_button_jiffies + msecs_to_jiffies(300))) {
        last_button_jiffies = current_jiffies;
        
        if (edge_capture & BUTTON_PLAY_PAUSE) {
            if (current_state == AUDIO_PLAYING) {
                audio_pause_chunked();
            } else {
                audio_play_chunked();
            }
        }
    }
    
    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    return (irq_handler_t) IRQ_HANDLED;
}

// Audio control functions
void audio_play_chunked(void)
{
    if (!file_open) {
        printk(KERN_ERR "File not open\n");
        return;
    }
    
    printk(KERN_INFO "Starting chunked playback\n");
    
    current_state = AUDIO_PLAYING;
    sample_counter = 0;
    
    // Load initial chunk if needed
    if (!chunk_ready) {
        chunk_ready = 0;
        load_next_chunk();
        
        if (!chunk_ready) {
            printk(KERN_ERR "Failed to load initial chunk\n");
            return;
        }
    }
    
    // Start background loader
    loader_should_run = 1;
    if (!loader_thread) {
        loader_thread = kthread_run(loader_thread_func, NULL, "audio_loader");
        if (IS_ERR(loader_thread)) {
            printk(KERN_ERR "Failed to create loader thread\n");
            loader_thread = NULL;
            return;
        }
    }
    
    // Initialize audio hardware
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    udelay(1000);
    iowrite32(0x1, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    // Start timers
    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(50));
    mod_timer(&display_timer, jiffies + HZ);
    
    display_time_mmss();
    
    printk(KERN_INFO "Chunked playback started\n");
}

void audio_pause_chunked(void)
{
    printk(KERN_INFO "Pausing chunked playback\n");
    current_state = AUDIO_PAUSED;
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    // Stop background loader
    loader_should_run = 0;
}

// Module initialization
static int __init initialize_audio_button_handler(void)
{
    int result;
    
    printk(KERN_INFO "=== Chunked Audio Player ===\n");
    printk(KERN_INFO "Chunk size: %d KB\n", CHUNK_SIZE / 1024);
    
    // Initialize variables
    current_state = AUDIO_STOPPED;
    file_open = 0;
    chunk_ready = 0;
    loading_in_progress = 0;
    loader_should_run = 0;
    loader_thread = NULL;
    last_button_jiffies = jiffies;
    
    // Initialize dual chunks
    active_chunk = chunk_a;
    loading_chunk = chunk_b;
    
    // Initialize timers
    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    
    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    
    // Map memory
    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    if (!lwbridgebase) {
        printk(KERN_ERR "Failed to map memory\n");
        return -ENOMEM;
    }
    
    // Initialize hardware
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();
    
    // Open audio file
    if (!open_audio_file_chunked()) {
        printk(KERN_ERR "Failed to open audio file\n");
        iounmap(lwbridgebase);
        return -EIO;
    }
    
    // Setup interrupts
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler, 
                        IRQF_SHARED, "audio_button_irq", 
                        (void *)(button_irq_handler));
    
    if (result) {
        printk(KERN_ERR "Failed to request IRQ: %d\n", result);
        if (audio_file) filp_close(audio_file, NULL);
        iounmap(lwbridgebase);
        return result;
    }
    
    printk(KERN_INFO "=== Chunked Audio Player Ready ===\n");
    
    return 0;
}

// Module cleanup
static void __exit cleanup_audio_button_handler(void)
{
    printk(KERN_INFO "=== Starting cleanup ===\n");
    
    current_state = AUDIO_STOPPED;
    loader_should_run = 0;
    
    // Stop loader thread
    if (loader_thread) {
        kthread_stop(loader_thread);
        loader_thread = NULL;
    }
    
    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);
    
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    if (audio_file) {
        filp_close(audio_file, NULL);
        audio_file = NULL;
    }
    
    iowrite32(0x0, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    free_irq(72 + 4, (void*)button_irq_handler);
    
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    
    if (lwbridgebase) {
        iounmap(lwbridgebase);
    }
    
    printk(KERN_INFO "=== Cleanup complete ===\n");
}

module_init(initialize_audio_button_handler);
module_exit(cleanup_audio_button_handler);