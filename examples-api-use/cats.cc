// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Dual Image Display with Clock for LED Matrix
// Displays two images side by side on a 64x32 LED matrix with time in middle
// Left image occupies columns 0-17, right image occupies columns 46-63
// Time display in middle gap (columns 18-45) - expanded for better readability
// Random facts scroll at bottom (rows 28-31) - updates every 5 minutes
//
// Dependencies:
//   sudo apt-get update
//   sudo apt-get install libgraphicsmagick++-dev libwebp-dev libcurl4-openssl-dev libjsoncpp-dev -y

#include "led-matrix.h"
#include "graphics.h"  // For text rendering

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <ctime>
#include <cstring>
#include <string>
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>

#include <exception>
#include <Magick++.h>
#include <magick/image.h>

#include <curl/curl.h>
#include <jsoncpp/json/json.h>

using rgb_matrix::Canvas;
using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;
using rgb_matrix::Font;
using rgb_matrix::Color;
using rgb_matrix::DrawText;

// Make sure we can exit gracefully when Ctrl-C is pressed.
volatile bool interrupt_received = false;
static void InterruptHandler(int signo) {
  interrupt_received = true;
}

using ImageVector = std::vector<Magick::Image>;

// Configuration for dual image layout on 64x32 matrix
const int MATRIX_WIDTH = 64;
const int MATRIX_HEIGHT = 32;
const int LEFT_IMAGE_WIDTH = 18;   
const int RIGHT_IMAGE_WIDTH = 18;  
const int LEFT_IMAGE_X = 0;       
const int RIGHT_IMAGE_X = 46;      
const int GAP_WIDTH = 28;         
const int CLOCK_X = 18;            
const int CLOCK_WIDTH = 28;        
const int IMAGE_HEIGHT = 21;       
const int IMAGE_Y = 1;            
const int WEATHER_Y = 28;          

// Global variables for fact management
std::string current_fact = "Loading today's fact...";
std::mutex fact_mutex;
std::atomic<bool> should_stop_fact_thread{false};

// CURL callback function
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Get today's date in Eastern timezone
std::string GetTodayDateLocal() {
  std::time_t now = std::time(nullptr);
  std::tm *local = std::localtime(&now);  // Uses system timezone (should be Eastern for you)
  char buf[11];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", local);
  return std::string(buf);
}

// Fetch or load today's fact (with caching)
std::string FetchOrLoadFactOfTheDay() {
  std::string today = GetTodayDateLocal();
  std::string cache_dir = "/tmp/cats-cache";
  std::filesystem::create_directories(cache_dir);
  std::string cache_path = cache_dir + "/" + today + ".txt";

  // Try to load from cache first
  std::ifstream infile(cache_path);
  if (infile.good()) {
    std::string cached_fact((std::istreambuf_iterator<char>(infile)),
                             std::istreambuf_iterator<char>());
    if (!cached_fact.empty()) {
      printf("Loaded cached fact for %s\n", today.c_str());
      return cached_fact;
    }
  }

  // If not cached or cache is empty, fetch from API
  printf("Fetching today's fact from API for %s...\n", today.c_str());
  
  CURL *curl;
  CURLcode res;
  std::string readBuffer;

  curl = curl_easy_init();
  if (!curl) {
    std::cerr << "Failed to initialize CURL" << std::endl;
    return "Could not fetch fact";
  }

  // Use today's fact endpoint for daily facts
  curl_easy_setopt(curl, CURLOPT_URL, "https://uselessfacts.jsph.pl/api/v2/facts/today");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "LED-Matrix-Facts/1.0");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

  res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
    return "Could not fetch today's fact";
  }

  // Parse JSON
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errs;
  std::istringstream s(readBuffer);

  if (!Json::parseFromStream(builder, s, &root, &errs)) {
    std::cerr << "JSON parse error: " << errs << std::endl;
    return "Error parsing today's fact";
  }

  if (!root.isMember("text")) {
    return "Today's fact not found in response";
  }

  std::string fact_text = root["text"].asString();
  
  // Clean up the fact text for LED display
  // Remove excessive whitespace and limit length
  std::replace(fact_text.begin(), fact_text.end(), '\n', ' ');
  std::replace(fact_text.begin(), fact_text.end(), '\r', ' ');
  
  // Remove multiple spaces
  std::string::size_type pos = 0;
  while ((pos = fact_text.find("  ", pos)) != std::string::npos) {
    fact_text.replace(pos, 2, " ");
  }
  
  // Trim and limit length for scrolling display
  fact_text.erase(0, fact_text.find_first_not_of(" "));
  fact_text.erase(fact_text.find_last_not_of(" ") + 1);
  
  if (fact_text.length() > 150) {
    fact_text = fact_text.substr(0, 147) + "...";
  }
  
  // Add prefix to make it clear it's a fact
  fact_text = "Today's fact: " + fact_text;

  // Save to cache
  std::ofstream outfile(cache_path);
  if (outfile.good()) {
    outfile << fact_text;
    outfile.close();
    printf("Cached today's fact for %s\n", today.c_str());
  }

  return fact_text;
}

std::string FetchFactWithRetry(int max_retries = 6, int wait_seconds = 10) {
    for (int attempt = 1; attempt <= max_retries; attempt++) {
        std::string fact = FetchOrLoadFactOfTheDay();  // your existing function
        if (fact.rfind("Today's fact:", 0) == 0) { 
            return fact; // valid fact
        }
        std::cerr << "Attempt " << attempt << " failed. Retrying in "
                  << wait_seconds << " seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(wait_seconds));
    }

    return "Could not fetch fact after retries"; // final fallback
}

// Background thread function to check for new day and update fact
void FactUpdateThread() {
  std::string last_date = "";
  
  while (!should_stop_fact_thread) {
    try {
      std::string current_date = GetTodayDateLocal();
      
      // Check if it's a new day
      if (current_date != last_date) {
        printf("New day detected: %s (was: %s)\n", current_date.c_str(), last_date.c_str());
        
        std::string new_fact = FetchFactWithRetry(6, 10);

        if (!new_fact.empty() && new_fact != "Could not fetch today's fact" && 
            new_fact != "Error parsing today's fact" && new_fact != "Today's fact not found in response") {
          
          // Thread-safe update of the current fact
          {
            std::lock_guard<std::mutex> lock(fact_mutex);
            current_fact = new_fact;
          }
          
          printf("Today's fact loaded for %s: %s\n", current_date.c_str(), new_fact.c_str());
        } else {
          printf("Failed to fetch today's fact, keeping current one\n");
        }
        
        last_date = current_date;
      }
    } catch (const std::exception& e) {
      printf("Exception in fact update: %s\n", e.what());
    }
    
    // Check every 30 minutes if it's a new day
    std::this_thread::sleep_for(std::chrono::minutes(30));
  }
}

// Thread-safe function to get current fact
std::string GetCurrentFact() {
  std::lock_guard<std::mutex> lock(fact_mutex);
  return current_fact;
}

// Load and scale a single image to specified dimensions
static ImageVector LoadImageAndScaleImage(const char *filename,
                                          int target_width,
                                          int target_height) {
  ImageVector result;

  ImageVector frames;
  try {
    readImages(&frames, filename);
  } catch (std::exception &e) {
    if (e.what())
      fprintf(stderr, "Error loading %s: %s\n", filename, e.what());
    return result;
  }

  if (frames.empty()) {
    fprintf(stderr, "No image found in %s\n", filename);
    return result;
  }

  // Animated images have partial frames that need to be put together
  if (frames.size() > 1) {
    Magick::coalesceImages(&result, frames.begin(), frames.end());
  } else {
    result.push_back(frames[0]); // just a single still image.
  }

  for (Magick::Image &image : result) {
    image.scale(Magick::Geometry(target_width, target_height));
  }

  printf("Loaded %s: %zu frame(s), scaled to %dx%d\n", 
         filename, result.size(), target_width, target_height);
  return result;
}

// Copy an image to a Canvas at specified position
void CopyImageToCanvas(const Magick::Image &image, Canvas *canvas, 
                       int offset_x, int offset_y) {
  // Copy all the pixels to the canvas at the specified offset
  for (size_t y = 0; y < image.rows() && y + offset_y < IMAGE_HEIGHT + IMAGE_Y; ++y) {
    for (size_t x = 0; x < image.columns(); ++x) {
      const Magick::Color &c = image.pixelColor(x, y);
      if (c.alphaQuantum() < 256) {
        canvas->SetPixel(x + offset_x, y + offset_y,
                         ScaleQuantumToChar(c.redQuantum()),
                         ScaleQuantumToChar(c.greenQuantum()),
                         ScaleQuantumToChar(c.blueQuantum()));
      }
    }
  }
}

// Helper function to determine if we are in the "dim" hours (EST)
bool IsDimHoursEST() {
  std::time_t now = std::time(nullptr);
  std::tm *local = std::localtime(&now);  // Uses system timezone

  int hour = local->tm_hour;
  int min = local->tm_min;
  // Dim between 23:30 and 8:00 EST
  if ((hour == 23 && min >= 30) || (hour < 8)) return true;
  return false;
}

// Draw current time in the middle gap with better spacing
void DrawClock(Canvas *canvas, const Font &font) {
  time_t rawtime;
  struct tm *timeinfo;
  char time_buffer[16];
  char date_buffer[16];
  
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  
  // Format time as HH:MM (24-hour format)
  strftime(time_buffer, sizeof(time_buffer), "%H:%M", timeinfo);
  // Format date as M/D (no leading zeros)
  strftime(date_buffer, sizeof(date_buffer), "%-m/%-d", timeinfo);
  
  // Colors for time display
  Color time_color(255, 255, 255);    // White for time
  Color date_color(180, 180, 180);    // Light gray for date
  
  // Calculate better centering with expanded gap
  int time_len = strlen(time_buffer);
  int date_len = strlen(date_buffer);
  
  // Center time in the expanded gap (approximate character width of 4 pixels)
  int time_x = CLOCK_X + (CLOCK_WIDTH - time_len * 4) / 2 - 2;
  int date_x = CLOCK_X + (CLOCK_WIDTH - date_len * 4) / 2 - 2;
  
  // Draw time higher up to make use of extra space
  DrawText(canvas, font, time_x, 10, time_color, time_buffer);
  
  // Draw date below time with more spacing
  DrawText(canvas, font, date_x, 18, date_color, date_buffer);
}

// Draw scrolling fact text at bottom
void DrawFactText(Canvas *canvas, const Font &font, const std::string &fact_text, int scroll_offset) {
  Color fact_color(100, 255, 100);  // Green for facts
  
  // Draw fact text with scrolling offset
  DrawText(canvas, font, scroll_offset, WEATHER_Y + 2, fact_color, fact_text.c_str());
}

// Utility function to compute string width in pixels for the font
int GetStringWidth(const Font& font, const std::string& text) {
  int width = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    width += font.CharacterWidth((uint8_t)text[i]);
  }
  return width;
}

// show static images on the matrix with clock and scrolling fact
void ShowDualStaticImagesWithClock(const Magick::Image &left_image, 
                                   const Magick::Image &right_image, 
                                   RGBMatrix *matrix,
                                   const Font &font,
                                   const Font &fact_font) {
    FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();
    int scroll_offset = MATRIX_WIDTH;
    std::string last_fact_text = "";
    int fact_width = 0;
    time_t last_brightness_check = 0;
    int last_brightness = -1; 
    std::string last_time_str = "";

    while (!interrupt_received) {
        time_t now = time(nullptr);
        
        // Check brightness only every 30 minutes (1800 seconds)
        if (now - last_brightness_check >= 1800) {
            int brightness = IsDimHoursEST() ? 10 : 100;
            if (brightness != last_brightness) {
                matrix->SetBrightness(brightness);
                last_brightness = brightness;
            }
            last_brightness_check = now;
        }

        // Get current time
        struct tm *timeinfo = localtime(&now);
        char time_buffer[16];
        strftime(time_buffer, sizeof(time_buffer), "%H:%M", timeinfo);
        std::string current_time_str(time_buffer);

        // Get current fact
        std::string current_fact_text = GetCurrentFact();

        // Check if fact changed
        if (current_fact_text != last_fact_text) {
            last_fact_text = current_fact_text;
            fact_width = GetStringWidth(fact_font, current_fact_text);
            scroll_offset = MATRIX_WIDTH;
        }

        // Only do full clear when time changes
        bool time_changed = (current_time_str != last_time_str);
        if (time_changed) {
            offscreen_canvas->Clear();
            last_time_str = current_time_str;
        }
        
        // ALWAYS redraw images and clock (they're static, so it's cheap)
        CopyImageToCanvas(left_image, offscreen_canvas, LEFT_IMAGE_X, IMAGE_Y);
        CopyImageToCanvas(right_image, offscreen_canvas, RIGHT_IMAGE_X, IMAGE_Y);
        DrawClock(offscreen_canvas, font);
        
        // Clear and redraw ONLY the scrolling text area
        for (int y = 20; y < MATRIX_HEIGHT; ++y) {
            for (int x = 0; x < MATRIX_WIDTH; ++x) {
                offscreen_canvas->SetPixel(x, y, 0, 0, 0);
            }
        }
        
        // Draw scrolling fact
        DrawFactText(offscreen_canvas, fact_font, current_fact_text, scroll_offset);
        
        // Swap buffers
        offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas);

        // Update scroll position
        scroll_offset--;
        if (scroll_offset < -fact_width) {
            scroll_offset = MATRIX_WIDTH;
        }

        usleep(8000);
    }
}

// Display two animated images with clock
// Display two animated images with clock
void ShowDualAnimatedImagesWithClock(const ImageVector &left_images,
                                     const ImageVector &right_images,
                                     RGBMatrix *matrix,
                                     const Font &font,
                                     const Font &fact_font) {
  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();
  size_t max_frames = std::max(left_images.size(), right_images.size());

  int scroll_offset = MATRIX_WIDTH;
  std::string last_fact_text = "";
  int fact_width = 0;
  int last_brightness = -1;
  time_t last_brightness_check = 0;
  std::string last_time_str;
  static int last_scroll_offset = -1;

  while (!interrupt_received) {
    for (size_t frame = 0; frame < max_frames; ++frame) {
      if (interrupt_received) break;
      
      time_t now = time(nullptr);
      // Check brightness only every 60 seconds
      if (now - last_brightness_check >= 60) {
        int brightness = IsDimHoursEST() ? 10 : 100;
        if (brightness != last_brightness) {
          matrix->SetBrightness(brightness);
          last_brightness = brightness;
        }
        last_brightness_check = now;
      }

      // --- Time ---
      time_t rawtime;
      struct tm *timeinfo;
      char time_buffer[16];
      time(&rawtime);
      timeinfo = localtime(&rawtime);
      strftime(time_buffer, sizeof(time_buffer), "%H:%M", timeinfo);
      std::string current_time_str(time_buffer);

      // --- Fact ---
      std::string current_fact_text = GetCurrentFact();

      // --- Fact scroll reset if changed ---
      if (current_fact_text != last_fact_text) {
        last_fact_text = current_fact_text;
        fact_width = GetStringWidth(fact_font, current_fact_text);
        scroll_offset = MATRIX_WIDTH;
      }

      // --- Only redraw if something changed ---
      if (current_time_str != last_time_str ||
        current_fact_text != last_fact_text ||
        scroll_offset != last_scroll_offset) {
        offscreen_canvas->Clear();
        const Magick::Image &left_frame = left_images[frame % left_images.size()];
        CopyImageToCanvas(left_frame, offscreen_canvas, LEFT_IMAGE_X, IMAGE_Y);
        const Magick::Image &right_frame = right_images[frame % right_images.size()];
        CopyImageToCanvas(right_frame, offscreen_canvas, RIGHT_IMAGE_X, IMAGE_Y);
        DrawClock(offscreen_canvas, font);
        DrawFactText(offscreen_canvas, fact_font, current_fact_text, scroll_offset);
        offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas);

        last_time_str = current_time_str;
        last_scroll_offset = scroll_offset;
      }

      // --- Scroll update ---
      scroll_offset--;
      if (scroll_offset < -fact_width) {
        scroll_offset = MATRIX_WIDTH;
      }

      // --- Frame delay ---
      int delay = 0;
      if (frame < left_images.size()) {
        delay = left_images[frame].animationDelay();
      }
      if (frame < right_images.size()) {
        int right_delay = right_images[frame].animationDelay();
        delay = (delay > 0) ? (delay + right_delay) / 2 : right_delay;
      }
      if (delay <= 0) delay = 10; // 100ms default

      usleep(delay * 10000);
    }
  }
}

int usage(const char *progname) {
  fprintf(stderr, "Usage: %s [led-matrix-options] <left-image> <right-image>\n", progname);
  fprintf(stderr, "\nDisplays two images side by side on a 64x32 LED matrix with clock and random facts\n");
  fprintf(stderr, "Left image: columns 0-17, Right image: columns 46-63 (20%% smaller)\n");
  fprintf(stderr, "Clock in expanded middle: columns 18-45\n");
  fprintf(stderr, "Random facts scroll at bottom: rows 28-31 (updates once daily at midnight EST)\n");
  fprintf(stderr, "Each image is scaled to 18x19 pixels\n\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Magick::InitializeMagick(*argv);

  // Initialize the RGB matrix
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  if (argc < 3) {
    return usage(argv[0]);
  }
  
  const char *left_filename = argv[1];
  const char *right_filename = argv[2];

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) {
    fprintf(stderr, "Failed to create matrix\n");
    return 1;
  }

  Font font;      // For clock and general text
  Font fact_font; // For scrolling fact text
  Font* fact_font_ptr = &fact_font;

  if (!font.LoadFont("/opt/cats-display/fonts/5x7.bdf")) { 
      fprintf(stderr, "Could not load font for clock!\n");
      return 1;
  }
  if (!fact_font.LoadFont("/opt/cats-display/fonts/6x13.bdf")) {
      fprintf(stderr, "Could not load font 6x13 for facts!\n");
      if (!fact_font.LoadFont("/opt/cats-display/fonts/6x9.bdf")) {
          fprintf(stderr, "Could not load font 6x9 for facts!\n");
          if (!fact_font.LoadFont("/opt/cats-display/fonts/5x8.bdf")) {
              fprintf(stderr, "Could not load large font 5x8 for facts. Using default.\n");
              fact_font_ptr = &font; // fallback to pointer
          }
      }
  }
  // Verify matrix dimensions
  if (matrix->width() != MATRIX_WIDTH || matrix->height() != MATRIX_HEIGHT) {
    fprintf(stderr, "Warning: Expected %dx%d matrix, got %dx%d\n", 
            MATRIX_WIDTH, MATRIX_HEIGHT, matrix->width(), matrix->height());
  }

  // Load both images (now scaled to 18x19 for 20% size reduction)
  ImageVector left_images = LoadImageAndScaleImage(left_filename, 
                                                  LEFT_IMAGE_WIDTH, 
                                                  IMAGE_HEIGHT);
  ImageVector right_images = LoadImageAndScaleImage(right_filename, 
                                                   RIGHT_IMAGE_WIDTH, 
                                                   IMAGE_HEIGHT);

  // Check if both images loaded successfully
  if (left_images.empty()) {
    fprintf(stderr, "Failed to load left image: %s\n", left_filename);
    delete matrix;
    return 1;
  }
  
  if (right_images.empty()) {
    fprintf(stderr, "Failed to load right image: %s\n", right_filename);
    delete matrix;
    return 1;
  }

  printf("Matrix: %dx%d, Left: %zu frames, Right: %zu frames\n",
         matrix->width(), matrix->height(), 
         left_images.size(), right_images.size());
  printf("Image dimensions: %dx%d, Middle gap: %d pixels wide\n",
         LEFT_IMAGE_WIDTH, IMAGE_HEIGHT, GAP_WIDTH);

  // Start the background fact update thread
  std::thread fact_thread(FactUpdateThread);
  
  std::string initial_fact = "Waiting for network... fact loading in background";

  {
    std::lock_guard<std::mutex> lock(fact_mutex);
    current_fact = initial_fact;
  }
  printf("Today's fact: %s\n", initial_fact.c_str());


  // Display the images based on whether they're animated or static
  bool left_animated = left_images.size() > 1;
  bool right_animated = right_images.size() > 1;
  
  if (!left_animated && !right_animated) {
    ShowDualStaticImagesWithClock(left_images[0], right_images[0], matrix, font, *fact_font_ptr);
  } else {
    ShowDualAnimatedImagesWithClock(left_images, right_images, matrix, font, *fact_font_ptr);
  }

  // Clean shutdown
  printf("Shutting down...\n");
  should_stop_fact_thread = true;
  fact_thread.join();
  
  matrix->Clear();
  delete matrix;
  curl_global_cleanup();
  return 0;
}
