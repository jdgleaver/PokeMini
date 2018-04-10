#include <stdio.h>
#include <stdint.h>
#include <boolean.h>
#ifdef _MSC_VER
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include <libretro.h>
#include <retro_miscellaneous.h>
/*
 bare functions to use
 int PokeMini_EmulateFrame(void);
 int PokeMini_Create(int flags, int soundfifo);
 void PokeMini_Destroy();
 int PokeMini_NewMIN(uint32_t size);
 int PokeMini_LoadMINFile(const char *filename);
 */

// PokeMini headers
#include "MinxIO.h"
#include "PMCommon.h"
#include "PokeMini.h"
#include "Hardware.h"
#include "Joystick.h"
#include "MinxAudio.h"
#include "UI.h"
#include "Video.h"
#include "Video_x4.h"

#define MAKEBTNMAP(btn,pkebtn) JoystickButtonsEvent((pkebtn), input_cb(0/*port*/, RETRO_DEVICE_JOYPAD, 0, (btn)) != 0)

// Sound buffer size
#define SOUNDBUFFER	2048
#define PMSOUNDBUFF	(SOUNDBUFFER*2)

// Screen dimension definitions
#define PM_SCEEN_WIDTH  96
#define PM_SCEEN_HEIGHT 64
#define PM_VIDEO_SCALE  4
#define PM_VIDEO_WIDTH  (PM_SCEEN_WIDTH * PM_VIDEO_SCALE)
#define PM_VIDEO_HEIGHT (PM_SCEEN_HEIGHT * PM_VIDEO_SCALE)

uint16_t screenbuff [PM_VIDEO_WIDTH * PM_VIDEO_HEIGHT];
int PixPitch = PM_VIDEO_WIDTH; // screen->pitch / 2;

// File path utils
static char g_basename[256];
static char *g_system_dir;
static char *g_save_dir;

// Platform menu (REQUIRED >= 0.4.4)
int UIItems_PlatformC(int index, int reason);
TUIMenu_Item UIItems_Platform[] = {
   PLATFORMDEF_GOBACK,
   { 0,  9, "Define Joystick...", UIItems_PlatformC },
   PLATFORMDEF_SAVEOPTIONS,
   PLATFORMDEF_END(UIItems_PlatformC)
};
int UIItems_PlatformC(int index, int reason)
{
   if (reason == UIMENU_OK)
      reason = UIMENU_RIGHT;
   if (reason == UIMENU_CANCEL)
      UIMenu_PrevMenu();
   if (reason == UIMENU_RIGHT)
   {
      if (index == 9)
         JoystickEnterMenu();
   }
   return 1;
}

static retro_log_printf_t log_cb = NULL;
static retro_video_refresh_t video_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_cb = NULL;
static retro_audio_sample_t audio_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
static retro_environment_t environ_cb = NULL;

// Utilities
///////////////////////////////////////////////////////////

// Taken from nestopia...
static void extract_basename(char *buf, const char *path, size_t size)
{
	const char *base = strrchr(path, '/');
	if (!base)
		base = strrchr(path, '\\');
	if (!base)
		base = path;
	
	if (*base == '\\' || *base == '/')
		base++;
	
	strncpy(buf, base, size - 1);
	buf[size - 1] = '\0';
	
	char *ext = strrchr(buf, '.');
	if (ext)
		*ext = '\0';
}

///////////////////////////////////////////////////////////

// Want this code to be 'minimally invasive'
// -> Will make use of existing PokeMini command line interface
//    wherever possible
static void InitialiseCommandLine(const struct retro_game_info *game)
{
	// Mandatory (?)
	CommandLineInit();
	
	// Set fixed overrides (i.e. these values will never change...)
	CommandLine.forcefreebios = 0; // OFF
	CommandLine.eeprom_share = 0;  // OFF (there is no practical benefit to a shared eeprom save
	                               //      - it just gets full and becomes a nuisance...)
	CommandLine.updatertc = 2;	    // Update RTC (0=Off, 1=State, 2=Host)
	CommandLine.sound = MINX_AUDIO_DIRECTPWM;
	CommandLine.joyenabled = 1;    // ON
	
	// Set default overrides (these should be settable via core options interface...)
	CommandLine.piezofilter = 1;  // ON
	CommandLine.lcdfilter = 1;	   // LCD Filter (0: nofilter, 1: dotmatrix, 2: scanline)
	CommandLine.lcdmode = 0;      // LCD Mode (0: analog, 1: 3shades, 2: 2shades)
	CommandLine.lcdcontrast = 64; // LCD contrast
	CommandLine.lcdbright = 0;    // LCD brightness offset
	CommandLine.palette = 0;      // Palette Index (1 - 14)
	// Palette values:
	//  0: Default
	//  1: Old
	//  2: Black & White
	//  3: Green Palette
	//  4: Green Vector
	//  5: Red Palette
	//  6: Red Vector
	//  7: Blue LCD
	//  8: LEDBacklight
	//  9: Girl Power
	// 10: Blue Palette
	// 11: Blue Vector
	// 12: Sepia
	// 13: Inv. B&W
	
	// Set file paths
	// > Handle Windows nonsense...
	char slash;
#if defined(_WIN32)
	slash = '\\';
#else
	slash = '/';
#endif
   // > Prep. work
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &g_system_dir))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Could not find system directory.\n");
	}
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &g_save_dir))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Could not find save directory.\n");
	}
   // > ROM path
	if (game->path != NULL)
	{
		// >> Set CommandLine.min_file (probably not needed, but whatever...)
		sprintf(CommandLine.min_file, "%s", game->path);
		// >> Set CommandLine.eeprom_file
		extract_basename(g_basename, game->path, sizeof(g_basename));
		sprintf(CommandLine.eeprom_file, "%s%c%s.eep", g_save_dir, slash, g_basename);
	}
	// > BIOS path
	// >> Set CommandLine.bios_file
	sprintf(CommandLine.bios_file, "%s%cbios.min", g_system_dir, slash);
}

///////////////////////////////////////////////////////////

// Load MIN ROM
int PokeMini_LoadMINFileXPLATFORM(size_t size, uint8_t* buffer)
{
	// Check if size is valid
	if ((size <= 0x2100) || (size > 0x200000))
		return 0;
	
	// Free existing color information
	PokeMini_FreeColorInfo();
	
	// Allocate ROM and set cartridge size
	if (!PokeMini_NewMIN(size))
		return 0;
	
	// Read content
	memcpy(PM_ROM,buffer,size);
	
	NewMulticart();
	
	return 1;
}

///////////////////////////////////////////////////////////

void handlekeyevents()
{
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_START,9);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_UP,10);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_DOWN,11);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_LEFT,4);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_RIGHT,5);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_A,1);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_B,2);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_L,6);
	MAKEBTNMAP(RETRO_DEVICE_ID_JOYPAD_R,7);
}

// Core functions
///////////////////////////////////////////////////////////

void *retro_get_memory_data(unsigned type)
{
	if (type == RETRO_MEMORY_SYSTEM_RAM)
		return PM_RAM;
	else
		return NULL;
}

///////////////////////////////////////////////////////////

size_t retro_get_memory_size(unsigned type)
{
	if (type == RETRO_MEMORY_SYSTEM_RAM)
		return 0x2000;
	else
		return 0;
}

///////////////////////////////////////////////////////////

void retro_set_video_refresh(retro_video_refresh_t cb)
{
	video_cb = cb;
}

///////////////////////////////////////////////////////////

void retro_set_audio_sample(retro_audio_sample_t cb)
{
	audio_cb = cb;
}

///////////////////////////////////////////////////////////

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
	audio_batch_cb = cb;
}

///////////////////////////////////////////////////////////

void retro_set_input_poll(retro_input_poll_t cb)
{
	poll_cb = cb;
}

///////////////////////////////////////////////////////////

void retro_set_input_state(retro_input_state_t cb)
{
	input_cb = cb;
}

///////////////////////////////////////////////////////////

void retro_set_environment(retro_environment_t cb)
{
	struct retro_log_callback logging;
	
	environ_cb = cb;
	
	if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
		log_cb = logging.log;
	else
		log_cb = NULL;
}

///////////////////////////////////////////////////////////

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->need_fullpath    = false;
	info->valid_extensions = "min";
	info->library_version  = "v0.60";
	info->library_name     = "PokeMini";
	info->block_extract    = false;
}

///////////////////////////////////////////////////////////

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	info->geometry.base_width   = PM_VIDEO_WIDTH;
	info->geometry.base_height  = PM_VIDEO_HEIGHT;
	info->geometry.max_width    = PM_VIDEO_WIDTH;
	info->geometry.max_height   = PM_VIDEO_HEIGHT;
	info->geometry.aspect_ratio = (float)PM_VIDEO_WIDTH / (float)PM_VIDEO_HEIGHT;
	info->timing.fps            = 72.0;
	info->timing.sample_rate    = 44100.0;
}

///////////////////////////////////////////////////////////

void retro_init (void)
{
	enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
	if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
		log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
}

///////////////////////////////////////////////////////////

void retro_deinit(void)
{
	PokeMini_VideoPalette_Free();
	PokeMini_Destroy();
}

///////////////////////////////////////////////////////////

void retro_reset (void)
{
	// Soft reset
	PokeMini_Reset(0);
}

///////////////////////////////////////////////////////////

void retro_run (void)
{
	int i;
	int offset = 0;
	static int16_t audiobuffer[612];
	static int16_t audiostretched[612 * 2];
	uint16_t audiosamples = 612;// MinxAudio_SamplesInBuffer();
	
	poll_cb();
	handlekeyevents();
	
	PokeMini_EmulateFrame();
	
	MinxAudio_GetSamplesS16(audiobuffer, audiosamples);
	for(i = 0;i < 612;i++)
	{
		audiostretched[offset]     = audiobuffer[i];
		audiostretched[offset + 1] = audiobuffer[i];
		offset += 2;
	}
	audio_batch_cb(audiostretched, audiosamples);
	
	if (PokeMini_Rumbling)
		PokeMini_VideoBlit((uint16_t *)screenbuff + PokeMini_GenRumbleOffset(PixPitch), PixPitch);
	else
		PokeMini_VideoBlit((uint16_t *)screenbuff, PixPitch);
	
	video_cb(screenbuff, PM_VIDEO_WIDTH, PM_VIDEO_HEIGHT, PM_VIDEO_WIDTH * 2/*Pitch*/);
}

///////////////////////////////////////////////////////////

size_t retro_serialize_size (void)
{
	return 0;
}

///////////////////////////////////////////////////////////

bool retro_serialize(void *data, size_t size)
{
	return false;
}

///////////////////////////////////////////////////////////

bool retro_unserialize(const void * data, size_t size)
{
	return false;
}

///////////////////////////////////////////////////////////

void retro_cheat_reset(void)
{
	// no cheats on this core
} 

///////////////////////////////////////////////////////////

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	// no cheats on this core
} 

///////////////////////////////////////////////////////////

bool retro_load_game(const struct retro_game_info *game)
{
	int passed;
	
	if (!game)
		return false;
	
	InitialiseCommandLine(game);
	
	//add LCDMODE_COLORS option
	// Set video spec and check if is supported
	if (!PokeMini_SetVideo((TPokeMini_VideoSpec *)&PokeMini_Video4x4, 16, CommandLine.lcdfilter, CommandLine.lcdmode))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Couldn't set video spec.\n");
		abort();
	}
	
	passed = PokeMini_Create(0/*flags*/, PMSOUNDBUFF); // returns 1 on completion,0 on error
	if (!passed)
		abort();
	
	PokeMini_VideoPalette_Init(PokeMini_RGB16, 1/*enablehighcolor*/);
	PokeMini_VideoPalette_Index(CommandLine.palette, NULL, CommandLine.lcdcontrast, CommandLine.lcdbright);
	PokeMini_ApplyChanges(); // Note: 'CommandLine.piezofilter' value is also read inside here
	
	PokeMini_UseDefaultCallbacks();
	
	MinxAudio_ChangeEngine(CommandLine.sound); // enable sound
	
	passed = PokeMini_LoadMINFileXPLATFORM(game->size, (uint8_t*)game->data); // returns 1 on completion,0 on error
	if (!passed)
		abort();
	
	// Hard reset (should this be soft...?)
	PokeMini_Reset(1);
	
	return true;
}

///////////////////////////////////////////////////////////

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return false;
}

///////////////////////////////////////////////////////////

void retro_unload_game(void)
{
	
}

// Useless (?) callbacks
///////////////////////////////////////////////////////////

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

///////////////////////////////////////////////////////////

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port;
	(void)device;
}

///////////////////////////////////////////////////////////

unsigned retro_get_region (void)
{ 
	return RETRO_REGION_NTSC;
}
