#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Fixed WAV Audio Player");

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

// WAV file structure
typedef struct {
    struct file *file;
    char filename[256];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t data_start_offset;
    uint32_t samples_played;
    uint32_t total_samples;
    uint8_t is_valid;
    loff_t current_pos;
} wav_file_t;

static wav_file_t songs[3];
static const char* song_paths[] = {
    "/media/sd/songs/song1.wav",
    "/media/sd/songs/song2.wav", 
    "/media/sd/songs/song3.wav"
};

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

// Audio buffer
#define AUDIO_BUFFER_SIZE 4096
static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
static int buffer_pos = 0;
static int buffer_size = 0;
static int buffer_needs_refill = 1;

// Simple sample rate matching
static int sample_skip_counter = 0;

// Workqueue for file operations
static struct workqueue_struct *audio_wq;
static struct work_struct load_work;
static struct work_struct refill_work;
static int next_track_to_load = 0;

// Debouncing
static struct timespec last_button_time[3];
#define DEBOUNCE_TIME_MS 200

// 7-segment patterns
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

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
void load_work_handler(struct work_struct *work);
void refill_work_handler(struct work_struct *work);
int parse_wav_header(wav_file_t* wav);
int read_wav_data_safe(wav_file_t* wav);

// FIXED WAV header parser
int parse_wav_header(wav_file_t* wav)
{
    uint8_t header[44];
    mm_segment_t oldfs;
    int bytes_read;
    uint32_t file_size;
    
    if (!wav->file) return 0;
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    wav->current_pos = 0;
    bytes_read = vfs_read(wav->file, header, 44, &wav->current_pos);
    
    set_fs(oldfs);
    
    if (bytes_read != 44) {
        printk(KERN_ERR "Failed to read WAV header (got %d bytes)\n", bytes_read);
        return 0;
    }
    
    // Verify RIFF/WAVE headers
    if (memcmp(header, "RIFF", 4) != 0) {
        printk(KERN_ERR "No RIFF header found\n");
        return 0;
    }
    
    if (memcmp(header + 8, "WAVE", 4) != 0) {
        printk(KERN_ERR "No WAVE header found\n");
        return 0;
    }
    
    // Extract file size from RIFF header
    file_size = *(uint32_t*)(header + 4);
    
    // Extract format information
    wav->channels = *(uint16_t*)(header + 22);
    wav->sample_rate = *(uint32_t*)(header + 24);
    wav->bits_per_sample = *(uint16_t*)(header + 34);
    
    // Find data chunk size (header + 40 in standard WAV)
    wav->data_size = *(uint32_t*)(header + 40);
    wav->data_start_offset = 44;  // Standard WAV data starts at byte 44
    
    // Validation
    if (wav->data_size == 0) {
        // Try to estimate from file size
        wav->data_size = file_size - 36;  // Total - header overhead
        printk(KERN_WARNING "Data size was 0, estimated: %u bytes\n", wav->data_size);
    }
    
    // Calculate total samples
    if (wav->channels > 0 && wav->bits_per_sample > 0) {
        uint32_t bytes_per_sample = wav->channels * (wav->bits_per_sample / 8);
        wav->total_samples = wav->data_size / bytes_per_sample;
    } else {
        wav->total_samples = 0;
    }
    
    wav->samples_played = 0;
    wav->current_pos = wav->data_start_offset;
    
    printk(KERN_INFO "WAV parsed: %uHz, %uch, %ubit\n", 
           wav->sample_rate, wav->channels, wav->bits_per_sample);
    printk(KERN_INFO "File size: %u KB, Data size: %u KB, Samples: %u\n",
           file_size / 1024, wav->data_size / 1024, wav->total_samples);
    
    return 1;
}

// Workqueue handler for loading files
void load_work_handler(struct work_struct *work)
{
    wav_file_t *wav;
    
    if (next_track_to_load < 0 || next_track_to_load >= total_tracks) 
        return;
    
    wav = &songs[next_track_to_load];
    
    // Close previous file
    if (wav->file) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
    }
    
    // Open new file
    wav->file = filp_open(song_paths[next_track_to_load], O_RDONLY, 0);
    if (IS_ERR(wav->file)) {
        printk(KERN_ERR "Cannot open %s (error %ld)\n", 
               song_paths[next_track_to_load], PTR_ERR(wav->file));
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    strcpy(wav->filename, song_paths[next_track_to_load]);
    
    if (!parse_wav_header(wav)) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    wav->is_valid = 1;
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    printk(KERN_INFO "Successfully loaded: %s\n", wav->filename);
}

// Workqueue handler for refilling buffer
void refill_work_handler(struct work_struct *work)
{
    wav_file_t *wav = &songs[current_track];
    mm_segment_t oldfs;
    int bytes_read;
    
    if (!wav->is_valid || !wav->file || !buffer_needs_refill) 
        return;
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    bytes_read = vfs_read(wav->file, audio_buffer, AUDIO_BUFFER_SIZE, &wav->current_pos);
    
    set_fs(oldfs);
    
    if (bytes_read > 0) {
        buffer_size = bytes_read;
        buffer_pos = 0;
        buffer_needs_refill = 0;
        
        printk(KERN_INFO "Buffer refilled: %d bytes\n", bytes_read);
    } else {
        printk(KERN_INFO "End of file reached\n");
        buffer_size = 0;
        buffer_pos = 0;
    }
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

// Audio generation with buffer management
void generate_audio_samples(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right;
    int i;
    wav_file_t *wav;
    
    if (current_state != AUDIO_PLAYING) return;
    
    wav = &songs[current_track];
    if (!wav->is_valid) return;
    
    fifospace = ioread32(lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_FIFOSPACE_REG);
    write_space_left = (fifospace >> 24) & 0xFF;
    write_space_right = (fifospace >> 16) & 0xFF;
    
    // Check if we need to refill buffer
    if (buffer_pos >= buffer_size && !buffer_needs_refill) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        return;
    }
    
    // Process samples
    for (i = 0; i < 5 && write_space_left > 0 && write_space_right > 0; i++) {
        uint32_t sample_32 = 0;
        uint8_t left_sample, right_sample;
        int16_t left_16, right_16;
        
        // Simple sample rate matching
        sample_skip_counter++;
        if (sample_skip_counter < 3) {
            continue;
        }
        sample_skip_counter = 0;
        
        // Check if we have data in buffer
        if (buffer_pos + 1 < buffer_size) {
            left_sample = audio_buffer[buffer_pos];
            right_sample = audio_buffer[buffer_pos + 1];
            buffer_pos += 2;
            wav->samples_played++;
        } else {
            // Use silence if no data available
            left_sample = 128;
            right_sample = 128;
        }
        
        // Convert with moderate amplification
        left_16 = ((int16_t)left_sample - 128) * 100;
        right_16 = ((int16_t)right_sample - 128) * 100;
        
        // Pack into 32-bit sample
        sample_32 = ((uint32_t)(left_16 & 0xFFFF) << 16) | (right_16 & 0xFFFF);
        
        // Write to audio hardware
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
        generate_audio_samples();
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));
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

// Debouncing
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
    
    printk(KERN_INFO "Button: 0x%x\n", edge_capture);
    
    if ((edge_capture & BUTTON_PLAY_PAUSE) && is_button_debounced(0)) {
        if (current_state == AUDIO_PLAYING) {
            audio_pause();
        } else {
            audio_play();
        }
    }
    
    if ((edge_capture & BUTTON_NEXT) && is_button_debounced(1)) {
        audio_next_track();
    }
    
    if ((edge_capture & BUTTON_PREV) && is_button_debounced(2)) {
        audio_prev_track();
    }
    
    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    return (irq_handler_t) IRQ_HANDLED;
}

// Audio control functions
void audio_play(void)
{
    printk(KERN_INFO "AUDIO: Starting playback - Track %d\n", current_track);
    
    current_state = AUDIO_PLAYING;
    sample_skip_counter = 0;
    
    // Ensure we have initial data
    if (songs[current_track].is_valid && buffer_size == 0) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        mdelay(100);  // Wait for initial buffer fill
    }
    
    // Initialize audio hardware
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    mdelay(1);
    iowrite32(0x1, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    mdelay(1);
    
    // Start timers
    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));
    mod_timer(&display_timer, jiffies + HZ);
    
    display_time_mmss();
}

void audio_pause(void)
{
    printk(KERN_INFO "AUDIO: Pausing\n");
    current_state = AUDIO_PAUSED;
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
}

void audio_next_track(void)
{
    int was_playing;
    
    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();
    
    current_track = (current_track + 1) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    // Reset buffer
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Next track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(200);
        audio_play();
    }
}

void audio_prev_track(void)
{
    int was_playing;
    
    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();
    
    current_track = (current_track - 1 + total_tracks) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;
    
    // Reset buffer
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Prev track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(200);
        audio_play();
    }
}

// Module initialization
static int __init initialize_audio_button_handler(void)
{
    int result;
    int i;
    struct timespec current_time;
    
    printk(KERN_INFO "=== Fixed WAV Audio Player ===\n");
    
    // Initialize debounce
    getnstimeofday(&current_time);
    for (i = 0; i < 3; i++) {
        last_button_time[i] = current_time;
    }
    
    // Create workqueue
    audio_wq = create_singlethread_workqueue("audio_wq");
    if (!audio_wq) {
        printk(KERN_ERR "Failed to create workqueue\n");
        return -ENOMEM;
    }
    
    INIT_WORK(&load_work, load_work_handler);
    INIT_WORK(&refill_work, refill_work_handler);
    
    // Initialize timers
    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    
    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    
    // Map memory
    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    if (!lwbridgebase) {
        destroy_workqueue(audio_wq);
        return -ENOMEM;
    }
    
    // Initialize hardware
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();
    
    // Load first song
    next_track_to_load = 0;
    queue_work(audio_wq, &load_work);
    mdelay(200);
    
    // Setup interrupts
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler, 
                        IRQF_SHARED, "audio_button_irq", 
                        (void *)(button_irq_handler));
    
    if (result) {
        iounmap(lwbridgebase);
        destroy_workqueue(audio_wq);
        return result;
    }
    
    printk(KERN_INFO "=== Fixed Audio Player Ready ===\n");
    
    return 0;
}

// Module cleanup
static void __exit cleanup_audio_button_handler(void)
{
    int i;
    
    current_state = AUDIO_STOPPED;
    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);
    
    iowrite32(0x0, lwbridgebase + AUDIO_BASE_OFFSET + AUDIO_CONTROL_REG);
    
    if (audio_wq) {
        flush_workqueue(audio_wq);
        destroy_workqueue(audio_wq);
    }
    
    for (i = 0; i < total_tracks; i++) {
        if (songs[i].file) {
            filp_close(songs[i].file, NULL);
        }
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
// Codigo con request_irq y ioremap_nocache
// Codigo con iowrite32 y vfs_read
// Codigo con filp_open y filp_close
// Codigo con get_fs y set_fs
// Codigo con mm_segment_t y loff_t