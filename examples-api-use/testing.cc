// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Dual Image Display with Clock for LED Matrix
// Displays two images side by side on a 64x32 LED matrix with time in middle
// Left image occupies columns 0-23, right image occupies columns 40-63
// Time display in middle gap (columns 24-39)
// Weather text scrolls at bottom (rows 28-31)
//
// Dependencies:
//   sudo apt-get update
//   sudo apt-get install libgraphicsmagick++-dev libwebp-dev -y

#include "led-matrix.h"
#include "graphics.h"  // For text rendering

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <ctime>
#include <string>
#include <iostream>

#include <exception>
#include <Magick++.h>
#include <magick/image.h>

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
const int LEFT_IMAGE_WIDTH = 24;   // Left image: columns 0-23
const int RIGHT_IMAGE_WIDTH = 24;  // Right image: columns 40-63  
const int LEFT_IMAGE_X = 0;
const int RIGHT_IMAGE_X = 40;
const int GAP_WIDTH = 16;          // Gap: columns 24-39
const int CLOCK_X = 24;            // Clock starts at column 24
const int CLOCK_WIDTH = 16;        // Clock area width
const int IMAGE_HEIGHT = 27;       // Images use rows 0-26, leave bottom for weather
const int WEATHER_Y = 28;          // Weather text starts at row 28

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
  for (size_t y = 0; y < image.rows() && y + offset_y < IMAGE_HEIGHT; ++y) {
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

// Draw current time in the middle gap
void DrawClock(Canvas *canvas, const Font &font) {
  time_t rawtime;
  struct tm *timeinfo;
  char time_buffer[16];
  char date_buffer[16];
  
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  
  // Format time as HH:MM
  strftime(time_buffer, sizeof(time_buffer), "%H:%M", timeinfo);
  // Format date as MM/DD
  strftime(date_buffer, sizeof(date_buffer), "%m/%d", timeinfo);
  
  // Colors for time display
  Color time_color(255, 255, 255);    // White for time
  Color date_color(180, 180, 180);    // Light gray for date
  
  // Draw time (centered in gap)
  int time_len = strlen(time_buffer);
  int time_x = CLOCK_X + (CLOCK_WIDTH - time_len * 4) / 2;  // Rough centering
  DrawText(canvas, font, time_x, 12, time_color, time_buffer);
  
  // Draw date below time
  int date_len = strlen(date_buffer);
  int date_x = CLOCK_X + (CLOCK_WIDTH - date_len * 4) / 2;  // Rough centering
  DrawText(canvas, font, date_x, 20, date_color, date_buffer);
}

// Draw scrolling weather text at bottom
void DrawWeatherText(Canvas *canvas, const Font &font, const std::string &weather_text, int scroll_offset) {
  Color weather_color(100, 255, 100);  // Green for weather
  
  // Draw weather text with scrolling offset
  DrawText(canvas, font, scroll_offset, WEATHER_Y + 3, weather_color, weather_text.c_str());
}

// Display two static images with clock
void ShowDualStaticImagesWithClock(const Magick::Image &left_image, 
                                  const Magick::Image &right_image, 
                                  RGBMatrix *matrix,
                                  const Font &font,
                                  const std::string &weather_text) {
  int scroll_offset = MATRIX_WIDTH;  // Start weather text off-screen right
  int weather_width = weather_text.length() * 4;  // Rough width calculation
  
  while (!interrupt_received) {
    matrix->Clear();
    
    // Draw images
    CopyImageToCanvas(left_image, matrix, LEFT_IMAGE_X, 0);
    CopyImageToCanvas(right_image, matrix, RIGHT_IMAGE_X, 0);
    
    // Draw clock
    DrawClock(matrix, font);
    
    // Draw scrolling weather
    DrawWeatherText(matrix, font, weather_text, scroll_offset);
    
    // Update scroll position
    scroll_offset--;
    if (scroll_offset < -weather_width) {
      scroll_offset = MATRIX_WIDTH;  // Reset to right side
    }
    
    usleep(100000);  // 100ms delay for smooth scrolling
  }
}

// Display two animated images with clock
void ShowDualAnimatedImagesWithClock(const ImageVector &left_images,
                                    const ImageVector &right_images,
                                    RGBMatrix *matrix,
                                    const Font &font,
                                    const std::string &weather_text) {
  FrameCanvas *offscreen_canvas = matrix->CreateFrameCanvas();
  
  // Determine the maximum number of frames between both animations
  size_t max_frames = std::max(left_images.size(), right_images.size());
  
  int scroll_offset = MATRIX_WIDTH;  // Start weather text off-screen right
  int weather_width = weather_text.length() * 4;  // Rough width calculation
  
  while (!interrupt_received) {
    for (size_t frame = 0; frame < max_frames; ++frame) {
      if (interrupt_received) break;
      
      offscreen_canvas->Clear();
      
      // Get current frame for left image (loop if necessary)
      const Magick::Image &left_frame = left_images[frame % left_images.size()];
      CopyImageToCanvas(left_frame, offscreen_canvas, LEFT_IMAGE_X, 0);
      
      // Get current frame for right image (loop if necessary)  
      const Magick::Image &right_frame = right_images[frame % right_images.size()];
      CopyImageToCanvas(right_frame, offscreen_canvas, RIGHT_IMAGE_X, 0);
      
      // Draw clock
      DrawClock(offscreen_canvas, font);
      
      // Draw scrolling weather
      DrawWeatherText(offscreen_canvas, font, weather_text, scroll_offset);
      
      // Update scroll position
      scroll_offset--;
      if (scroll_offset < -weather_width) {
        scroll_offset = MATRIX_WIDTH;  // Reset to right side
      }
      
      offscreen_canvas = matrix->SwapOnVSync(offscreen_canvas);
      
      // Use the delay from whichever image has more frames, or average them
      int delay = 0;
      if (frame < left_images.size()) {
        delay = left_images[frame].animationDelay();
      }
      if (frame < right_images.size()) {
        int right_delay = right_images[frame].animationDelay();
        delay = (delay > 0) ? (delay + right_delay) / 2 : right_delay;
      }
      
      // Default delay if none specified, but minimum for smooth scrolling
      if (delay <= 0) delay = 10; // 100ms default
      
      usleep(delay * 10000);  // 1/100s converted to usec
    }
  }
}

int usage(const char *progname) {
  fprintf(stderr, "Usage: %s [led-matrix-options] <left-image> <right-image> [weather-text]\n", progname);
  fprintf(stderr, "\nDisplays two images side by side on a 64x32 LED matrix with clock and weather\n");
  fprintf(stderr, "Left image: columns 0-23, Right image: columns 40-63\n");
  fprintf(stderr, "Clock in middle: columns 24-39\n");
  fprintf(stderr, "Weather text scrolls at bottom: rows 28-31\n");
  fprintf(stderr, "Each image is scaled to 24x27 pixels\n\n");
  rgb_matrix::PrintMatrixFlags(stderr);
  return 1;
}

int main(int argc, char *argv[]) {
  Magick::InitializeMagick(*argv);

  // Initialize the RGB matrix
  RGBMatrix::Options matrix_options;
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }

  if (argc < 3 || argc > 4) {
    return usage(argv[0]);
  }
  
  const char *left_filename = argv[1];
  const char *right_filename = argv[2];
  std::string weather_text = (argc == 4) ? argv[3] : "Sunny 72°F - Light breeze from the west";

  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

  RGBMatrix *matrix = RGBMatrix::CreateFromOptions(matrix_options, runtime_opt);
  if (matrix == NULL) {
    fprintf(stderr, "Failed to create matrix\n");
    return 1;
  }

  // Load font for text rendering
  Font font;
  if (!font.LoadFont("../fonts/4x6.bdf")) {
    fprintf(stderr, "Couldn't load font '../fonts/4x6.bdf'\n");
    fprintf(stderr, "Trying alternative font paths...\n");
    if (!font.LoadFont("fonts/4x6.bdf") && 
        !font.LoadFont("/usr/share/fonts/misc/4x6.bdf")) {
      fprintf(stderr, "Could not load any font. Text will not display.\n");
      // Continue without font - images will still show
    }
  }

  // Verify matrix dimensions
  if (matrix->width() != MATRIX_WIDTH || matrix->height() != MATRIX_HEIGHT) {
    fprintf(stderr, "Warning: Expected %dx%d matrix, got %dx%d\n", 
            MATRIX_WIDTH, MATRIX_HEIGHT, matrix->width(), matrix->height());
  }

  // Load both images (now scaled to 24x27 to leave room for weather)
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
  printf("Weather: %s\n", weather_text.c_str());

  // Display the images based on whether they're animated or static
  bool left_animated = left_images.size() > 1;
  bool right_animated = right_images.size() > 1;
  
  if (!left_animated && !right_animated) {
    // Both images are static
    ShowDualStaticImagesWithClock(left_images[0], right_images[0], matrix, font, weather_text);
  } else {
    // At least one image is animated
    ShowDualAnimatedImagesWithClock(left_images, right_images, matrix, font, weather_text);
  }

  matrix->Clear();
  delete matrix;
  return 0;
}
