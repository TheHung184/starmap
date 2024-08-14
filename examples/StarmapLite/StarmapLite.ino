// StarmapTFT with GeoIP
// rev 1 - August 2024 - modified to use GeoIP instead of GPS
// rev 2 - August 2024 - modified to use flash stored Yale map

// ******** includes ********
#include <Starmap.h>
#include "SPI.h"
#include "Adafruit_GFX.h"
#include "Adafruit_GC9A01A.h"
#include <WiFi.h>
#include <WiFiMulti.h>
#include <GeoIP.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include "yale_array.h"
// ******** defines ********

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org");
unsigned long lastTimeUpdate = 0;
#define FOREVER 1
#define DELAY_MS delay
// time offset, example: 1 hour ahead of UTC (e.g. British Summer Time) is 1
#define DISPLAYED_TIME_OFFSET 1
// screen dimensions
#define TFT_W 240
#define TFT_H 240
#define DEFAULT_LAT 47.0
#define DEFAULT_LON 122.0
// TFT connections
#define TFT_DC  1
#define TFT_CS 5
#define TFT_RST 0
// hardware SPI CS must be set to the same as TFT_CS
#define DEFAULT_CS TFT_CS
// Flash memory Chip Select (manually controlled CS)
#define FLASH_CS 6
// Flash memory command to read data
#define FLASH_READ 0x03
#define DATA_BUF_SIZE 256

//10x12 font for N,E,S,W characters only
const uint16_t font10_12[4][12] = {
    {0x0fff, 0x0fff, 0x0700, 0x03c0, 0x01e0, 0x0078, 0x003c, 0x000e, 0x0fff, 0x0fff},
    {0x0fff, 0x0fff, 0x0c63, 0x0c63, 0x0c63, 0x0c63, 0x0c63, 0x0c63, 0x0c03, 0x0c03},
    {0x038c, 0x07ce, 0x0ee7, 0x0c63, 0x0c63, 0x0c63, 0x0c63, 0x0e77, 0x073e, 0x031c},
    {0x0f80, 0x0ff8, 0x00ff, 0x0007, 0x007e, 0x007e, 0x0007, 0x00ff, 0x0ff8, 0x0f80}
};
#define NORTH_SYMBOL 0
#define EAST_SYMBOL 1
#define SOUTH_SYMBOL 2
#define WEST_SYMBOL 3
#define NESW_COLOR 0xf800
// misc
#ifndef PI
#define PI 3.14159265358979323846
#endif
// Modify the tm_t structure to be compatible with the standard tm structure


// ***** class based on Starmap ******
class SM : public Starmap {
  // you need to implement plot_pixel and/or draw_line
  // if you want text output, then implement either plot_pixel or text_out
  void plot_pixel(uint16_t color, int x, int y);
  void draw_line(int x0, int y0, int x1, int y1, uint16_t color);
  // optionally you can also implement text_out
  // void text_out(int x, int y, char* lab, unsigned char len, char type);
  // if star plotting is required, then storage_read is implemented
  int storage_read(uint32_t addr, char* data, uint16_t len);
};

// ******** global variables ********
// starmap and tft objects and GeoIP object
SM starmap;
Adafruit_GC9A01A tft(TFT_CS, TFT_DC,TFT_RST);
GeoIP geoip;
WiFiMulti wifiMulti;
location_t loc;

double mag;
rect_s screen_rect;
tm_t tm;
char first_fix_done = 0;
char flash_present = 0; // set to 1 if the flash is present and valid
char flash_data[DATA_BUF_SIZE]; // buffer used for storing data read from Flash
uint32_t flash_data_addr; // starting address of the contents of flash_data
uint32_t starmap_update_period = 5 * 60000; // multiply by 60000 to convert minutes to milliseconds

// ******** function prototypes ************
// void flash_read(uint32_t addr, char *data, uint16_t len);
void draw_flag(int x, int y, int flagtype);
void ccw_azimuth_elevation_to_xy(double az, double el, int *x, int *y);
void get_first_fix(void);
// int copy_gnss_time_to_starmap(void);
// int copy_gnss_loc_to_starmap(void);
int sync_time_with_ntp_and_geoip(void);
int copy_geoip_loc_to_starmap(void);
void read_yale_data(uint32_t addr, char* data, uint16_t len);
void setup_wifi_and_geoip(void);
// void invalidate_displayed_sat_list(void);
// int check_and_add_sat(uint8_t id);
void plot_char_10_12(char c, int x, int y, int color); // only supports N, E, S, W characters
void disp_lat_lon(double lat, double lon, int x, int y, int col);

// ******* plot_pixel function ******
void SM::draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
  // sanity check
  if (x0<0) x0=0;
  if (x1<0) x1=0;
  if (y0<0) y0=0;
  if (y1<0) y1=0;
  if (x0>=TFT_W) x0=TFT_W-1;
  if (x1>=TFT_W) x1=TFT_W-1;
  if (y0>=TFT_H) y0=TFT_H-1;
  if (y1>=TFT_H) y1=TFT_H-1;
  // handle your TFT here
  tft.drawLine(x0, y0, x1, y1, color);
}

void SM::plot_pixel(uint16_t color, int x, int y) {
    // sanity check
    if (x<0) x=0;
    if (y<0) y=0;
    if (x>=TFT_W) x=TFT_W-1;
    if (y>=TFT_H) y=TFT_H-1;
    // handle your TFT here
    tft.drawPixel(x, y, color);
}

// ******* storage_read function *******
int SM::storage_read(uint32_t addr, char* data, uint16_t len){
    // read from Flash memory
    read_yale_data(addr, data, len);
    return 0;
}

// ******** setup() function ********
void setup() {
  // put your setup code here, to run once:
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  Serial.begin(115200);
  // GPS setup
  setup_wifi_and_geoip();
    // Initialize NTP Client
  timeClient.begin();
  timeClient.setUpdateInterval(60000); // Update every 60 seconds
  // starmap setup
  mag = 5; // render magnitude level
  screen_rect.left = 0;
  screen_rect.right = TFT_W;
  screen_rect.top = 0;
  screen_rect.bottom = TFT_H;
  starmap.siteLat = DEFAULT_LAT;
  starmap.siteLon = DEFAULT_LON;
  // colors
  starmap.col_coord_grid = 0x4a49; // dark gray
  starmap.col_ecliptic = 0xab91; // dark pink-red
  starmap.col_constel = 0x326b; // dark aqua
  starmap.col_stardim = 0xa520; // dim yellow
  starmap.col_starbright = 0xffe0; // bright yellow
  starmap.col_startext = 0x001f; // pure blue
  starmap.col_moon_bright = 0xffff; // white
  starmap.col_moon_dim = 0xe71c; // light gray
  starmap.col_moon_dark = 0xce79; // med gray
  starmap.col_moon_phtext = 0x0000; // black
  starmap.col_bright_st_text = 0x001f; // pure blue
  starmap.col_ecliptic_text = 0xab91; // dark pink-red
  starmap.col_celest_eq_text = 0x4a49; // dark gray
  starmap.col_constel_text = 0x0030; // dark blue

  // tft setup
  tft.begin();
  // setup the Flash chip select pin
  flash_present=1;

  DELAY_MS(3000);
  Serial.print("setup() complete\n\r");
  if(flash_present) {
    Serial.print("flash present and valid\n\r");
  }
}

// ******** loop() function ********
void loop() {
  double lat, lon;
  int valid=0;
  int i;
  int x, y;
  uint32_t ts; // timestamp in milliseconds
  int hr, min, sec; // used to store the displayed time
  int old_min;
  char disp_lat_lon_complete;
  char text_string[24];

  if (!first_fix_done) {
    // Get location from GeoIP
    tft.fillScreen(GC9A01A_BLACK); // clear the TFT screen
    tft.setCursor(20, 120);
    tft.setTextColor(GC9A01A_WHITE);
    tft.setTextSize(1);
    tft.println("Acquiring Location and Time..");
    yield();
    loc = geoip.getGeoFromWiFi(true);
    sync_time_with_ntp_and_geoip();
    first_fix_done = 1;
  }
    // Periodically update time
  if (millis() - lastTimeUpdate > 60000) { // Update every minute
    sync_time_with_ntp_and_geoip();
    lastTimeUpdate = millis();
  }
  // fill starmap object with the current time and GeoIP location
  copy_geoip_loc_to_starmap();
  
  // Use current system time
  // time_t now = time(nullptr);
  // struct tm *timeinfo = localtime(&now);

  // // Copy the values to our tm_t structure
  // tm.tm_sec = timeinfo->tm_sec;
  // tm.tm_min = timeinfo->tm_min;
  // tm.tm_hour = timeinfo->tm_hour;
  // tm.tm_mday = timeinfo->tm_mday;
  // tm.tm_mon = timeinfo->tm_mon;
  // tm.tm_year = timeinfo->tm_year;
  // tm.tm_wday = timeinfo->tm_wday;
  // tm.tm_yday = timeinfo->tm_yday;
  // tm.tm_isdst = timeinfo->tm_isdst;
  starmap.jdtime = starmap.jtime(&tm);

  tft.fillScreen(GC9A01A_BLACK); // clear the TFT screen
  yield();
  Serial.print("executing paintSky..\n\r");
  starmap.paintSky(mag, &screen_rect); // paint the sky!
  yield();
  Serial.print("done executing paintSky.\n\r");

  // print lat and lon on tft
    lat = starmap.siteLat;
    lon = starmap.siteLon;
    disp_lat_lon(lat, lon, 52, 190, GC9A01A_WHITE);

  // invalidate_displayed_sat_list(); // clear the list of displayed satellites
  old_min = -1;
  disp_lat_lon_complete = 0;
  // loop until the starmap needs to be redrawn
  ts = millis(); // get current Arduino timestamp
  while (millis() - ts < starmap_update_period) {

    if (disp_lat_lon_complete==0) {
       if ((millis() - ts > 10000)) { // display the lat and lon for 10 seconds
           tft.fillScreen(GC9A01A_BLACK); // clear the TFT screen
           starmap.paintSky(mag, &screen_rect);
           yield();
           disp_lat_lon_complete = 1;
       }
    }
    hr = tm.tm_hour;
    min = tm.tm_min;
    sec = tm.tm_sec;
            plot_char_10_12(NORTH_SYMBOL, 115, 18, NESW_COLOR);
            plot_char_10_12(EAST_SYMBOL, 3, 125, NESW_COLOR);
            plot_char_10_12(SOUTH_SYMBOL, 115, 234, NESW_COLOR);
            plot_char_10_12(WEST_SYMBOL, 225, 125, NESW_COLOR);
            // display lat and lon
            // lat = starmap.siteLat;
            // lon = starmap.siteLon;
            // disp_lat_lon(lat, lon, 52, 190, GC9A01A_WHITE);
    //     }
    // }
    // display the time if it has changed
    if (min != old_min) {
        old_min = min;
        sprintf(text_string, "%02d:%02d", hr, min);
        tft.fillRect(91, 199, 60, 16, GC9A01A_BLACK);
        tft.setCursor(92, 200);
        tft.setTextColor(GC9A01A_WHITE);
        tft.setTextSize(2);
        tft.println(text_string);
        yield();
    }
  } // end while loop for starmap_update_period


  // starmap has been displayed for starmap_update_period
  // now loop back to the beginning, so that the starmap is completely redrawn
}

// ************ other functions *****************
// Add a new function to read from the yale_array
void read_yale_data(uint32_t addr, char* data, uint16_t len) {
  // addr = addr - 0x08c00;
  memcpy_P(data, yale_array + addr - 0x08c00, len);
}
// New function to setup WiFi and GeoIP
void setup_wifi_and_geoip() {
  WiFi.mode(WIFI_STA);
  
  wifiMulti.addAP(ssid1, password1);
  wifiMulti.addAP(ssid2, password2);

  Serial.println("\nConnecting to WiFi...");
  
  while (wifiMulti.run() != WL_CONNECTED) {
    Serial.print('.');
    delay(100);
  }

  Serial.print("\nConnected to ");
  Serial.println(WiFi.SSID());
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

// Modified function to copy GeoIP location to Starmap object
int copy_geoip_loc_to_starmap(void) {
  if (!loc.status) {
    Serial.println("Error: Invalid GeoIP location");
    return -1;
  }

  starmap.siteLat = loc.latitude;
  starmap.siteLon = -loc.longitude; // Starmap uses degrees West

  Serial.print("Location set to Lat: ");
  Serial.print(starmap.siteLat);
  Serial.print(", Lon: ");
  Serial.println(-starmap.siteLon);

  return 1;
}

// plot_char_10_12 only supports N, E, S, W characters
void plot_char_10_12(char c, int x, int y, int color) {
    int i, j;
    // check that the character is in the font
    // only 0-3 are valid (N, E, S, W)
    if (c > 3 || c < 0)
    {
        return;
    }

    for (i = 0; i < 10; i++)
    {
        for (j = 0; j < 12; j++)
        {
            if (font10_12[c][i] & (1 << j))
            {
                tft.drawPixel(x + i, y - j, color);
            } else {
                tft.drawPixel(x + i, y - j, GC9A01A_BLACK);
            }
        }
    }
}

// function to convert elevation and azimuth to x,y coordinates
// with north-up, but in a counter-clockwise fashion, since we are 
// plotting an overhead view of the sky
void ccw_azimuth_elevation_to_xy(double az, double el, int *x, int *y) {
  // convert to radians
  az = az * PI / 180.0;
  el = el * PI / 180.0;
  // calculate x and y
  *x = (int)(TFT_W/2 + (TFT_W/2-1) * cos(el) * sin(az));
  *y = (int)(TFT_H/2 - (TFT_H/2-1) * cos(el) * cos(az));
  // we want anticlockwise rotation
  *x = TFT_W - *x;
}


int sync_time_with_ntp_and_geoip() {
  timeClient.update();

  // Get the UTC time from NTP
  time_t utc_time = timeClient.getEpochTime();

  // Apply the timezone offset from GeoIP
  time_t local_time = utc_time + loc.offsetSeconds;

  // Convert to tm structure
  struct tm *timeinfo = localtime(&local_time);

  // Copy the values to our tm_t structure
  tm.tm_sec = timeinfo->tm_sec;
  tm.tm_min = timeinfo->tm_min;
  tm.tm_hour = timeinfo->tm_hour;
  tm.tm_mday = timeinfo->tm_mday;
  tm.tm_mon = timeinfo->tm_mon;
  tm.tm_year = timeinfo->tm_year;
  tm.tm_wday = timeinfo->tm_wday;
  tm.tm_yday = timeinfo->tm_yday;
  tm.tm_isdst = timeinfo->tm_isdst;

  // Store the time in Julian days in the starmap object
  starmap.jdtime = starmap.jtime(&tm);

  Serial.print("Current local time: ");
  Serial.print(tm.tm_year + 1900); // Year since 1900
  Serial.print("-");
  Serial.print(tm.tm_mon + 1); // Month (0-11)
  Serial.print("-");
  Serial.print(tm.tm_mday); // Day of the month
  Serial.print(" ");
  Serial.print(tm.tm_hour); // Hour (0-23)
  Serial.print(":");
  Serial.print(tm.tm_min); // Minute
  Serial.print(":");
  Serial.println(tm.tm_sec); // Second

  return 1;
}

void disp_lat_lon(double lat, double lon, int x, int y, int col) {
    char text_string[32];
    char neg_lat=0;
    char neg_lon=0;
    int width;
    if (lat < 0) {
        neg_lat = 1;
        lat = 0 - lat;
    }
    if (lon < 0) {
        neg_lon = 1;
        lon = 0 - lon;
    }
    // build text string; there must be an easier way to do this!
    if (neg_lat) {
        if (neg_lon) {
            sprintf(text_string, "LAT:-%02d.%03dN LON:-%02d.%03dW", (int)lat, (int)((lat - (int)lat) * 1000), (int)lon, (int)((lon - (int)lon) * 1000));
        } else {
            sprintf(text_string, "LAT:-%02d.%03dN LON:%02d.%03dW", (int)lat, (int)((lat - (int)lat) * 1000), (int)lon, (int)((lon - (int)lon) * 1000));
        }
    } else {
        if (neg_lon) {
            sprintf(text_string, "LAT:%02d.%03dN LON:-%02d.%03dW", (int)lat, (int)((lat - (int)lat) * 1000), (int)lon, (int)((lon - (int)lon) * 1000));
        } else {
            sprintf(text_string, "LAT:%02d.%03dN LON:%02d.%03dW", (int)lat, (int)((lat - (int)lat) * 1000), (int)lon, (int)((lon - (int)lon) * 1000));
        }
    }

    width = 158;
    if (neg_lat) width += 8;
    if (neg_lon) width += 8;
    tft.fillRect(x-1, y-1, width, 10, GC9A01A_BLACK);
    tft.setCursor(x, y);
    tft.setTextColor(col);
    tft.setTextSize(1);
    tft.println(text_string);
}
