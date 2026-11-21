// -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
//
// Rotating portraits with a clock, date, and daily fact on a 64x32 LED matrix.
//
// Dependencies:
//   sudo apt-get update
//   sudo apt-get install libgraphicsmagick++-dev libwebp-dev libcurl4-openssl-dev libjsoncpp-dev -y

#ifdef CATS_PREVIEW
#include "cats-preview.h"
#else
#include "led-matrix.h"
#endif
#include "graphics.h"  // For text rendering

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <string>
#include <fstream>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>

#include <exception>
#include <Magick++.h>

#include <curl/curl.h>
#ifdef CATS_PREVIEW
#include <json/json.h>
using CatsMatrix = PreviewMatrix;
using CatsFrame = PreviewMatrix::Frame;
#else
#include <jsoncpp/json/json.h>
using CatsMatrix = rgb_matrix::RGBMatrix;
using CatsFrame = rgb_matrix::FrameCanvas;
#endif

#ifndef CATS_FONT_DIR
#define CATS_FONT_DIR "/opt/cats-display/fonts"
#endif

using rgb_matrix::Canvas;
using rgb_matrix::Font;
using rgb_matrix::Color;
using rgb_matrix::DrawText;

using DisplayClock = std::chrono::steady_clock;

// A pipe wakes the renderer from both the fact thread and the signal handler.
// The descriptors stay open for the life of the process.
static int display_wakeup[2];
static std::atomic<bool> interrupt_received{false};
static_assert(std::atomic<bool>::is_always_lock_free);

static bool InitializeDisplayWakeup() {
  if (pipe(display_wakeup) != 0) return false;
  for (int &fd : display_wakeup) {
    // The driver's daemon mode can replace standard I/O descriptors.
    if (fd <= STDERR_FILENO) {
      const int replacement = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
      if (replacement < 0) return false;
      close(fd);
      fd = replacement;
    }
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
  }
  return true;
}

static void WakeDisplay() {
  const int saved_errno = errno;
  const char byte = 0;
  while (write(display_wakeup[1], &byte, 1) < 0 && errno == EINTR) {}
  // A full pipe already has a wakeup pending.
  errno = saved_errno;
}

static void InterruptHandler(int) {
  interrupt_received = true;
  WakeDisplay();
}

enum class DisplayWake { NoNotification, Notification, Error };

static DisplayWake WaitForDisplayUpdate(DisplayClock::time_point deadline) {
  using namespace std::chrono;
  const auto delay = ceil<milliseconds>(deadline - DisplayClock::now());
  pollfd wakeup{display_wakeup[0], POLLIN, 0};
  const int ready = poll(&wakeup, 1, std::max(delay, milliseconds::zero()).count());
  if ((ready < 0 && errno != EINTR) || (wakeup.revents & (POLLERR | POLLHUP | POLLNVAL))) {
    if (ready < 0) perror("Could not wait for display update");
    else fprintf(stderr, "Display wakeup pipe closed unexpectedly\n");
    return DisplayWake::Error;
  }
  if (ready <= 0) return DisplayWake::NoNotification;

  char pending[64];
  ssize_t bytes;
  do {
    bytes = read(display_wakeup[0], pending, sizeof(pending));
  } while (bytes > 0 || (bytes < 0 && errno == EINTR));
  if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return DisplayWake::Notification;
  }
  if (bytes < 0) perror("Could not read display update");
  else fprintf(stderr, "Display wakeup pipe closed unexpectedly\n");
  return DisplayWake::Error;
}

using ImageVector = std::vector<Magick::Image>;

// Pre-rendered pixel data to avoid expensive GraphicsMagick pixelColor()
// calls on every frame. Pixels are converted once at startup.
struct Pixel {
  uint8_t r, g, b;
};

struct PreRenderedImage {
  std::vector<Pixel> pixels;
  int width, height;
  int animation_delay;  // GIF frame delay in centiseconds (preserved from GM)
};

using PreRenderedFrames = std::vector<PreRenderedImage>;

constexpr int MATRIX_WIDTH = 64;
constexpr int MATRIX_HEIGHT = 32;
constexpr int PORTRAIT_X = 1;
constexpr int PORTRAIT_Y = 1;
constexpr int PORTRAIT_WIDTH = 32;
constexpr int PORTRAIT_HEIGHT = 22;
constexpr int CLOCK_X = PORTRAIT_WIDTH + 2;
constexpr int CLOCK_WIDTH = MATRIX_WIDTH - CLOCK_X;
constexpr int CLOCK_BASELINE = 13;
constexpr int DATE_BASELINE = 21;
constexpr int FACT_BASELINE = 31;

#ifndef CATS_PREVIEW
static CatsMatrix::Options DefaultMatrixOptions() {
  CatsMatrix::Options options;
  options.rows = MATRIX_HEIGHT;
  options.cols = MATRIX_WIDTH;
  return options;
}
#endif

constexpr auto PORTRAIT_DURATION = std::chrono::seconds(8);
constexpr auto SCROLL_PAUSE = std::chrono::seconds(1);
constexpr auto SCROLL_STEP = std::chrono::microseconds(29'400);
constexpr int SCROLL_GAP = 12;
constexpr size_t MAX_FACT_RESPONSE_BYTES = 16 * 1024;
constexpr char FACT_PREFIX[] = "Today's fact: ";

// Global variables for fact management
std::string current_fact = "Waiting for network... fact loading in background";
std::mutex fact_mutex;
std::atomic<bool> should_stop_fact_thread{false};
std::condition_variable fact_stop_cv;
std::mutex fact_stop_mutex;

static size_t WriteFactResponse(char *contents, size_t size, size_t count, void *user_data) {
  if (size == 0 || count == 0) return 0;
  auto &response = *static_cast<std::string *>(user_data);
  if (response.size() >= MAX_FACT_RESPONSE_BYTES ||
      count > (MAX_FACT_RESPONSE_BYTES - response.size()) / size) {
    fprintf(stderr, "Daily fact response exceeded %zu bytes\n", MAX_FACT_RESPONSE_BYTES);
    return 0;
  }

  const size_t bytes = size * count;
  response.append(contents, bytes);
  return bytes;
}

static int AbortFactRequest(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return should_stop_fact_thread.load() ? 1 : 0;
}

struct DailyFact {
  std::string day;
  std::string id;
  std::string text;
};

static const std::filesystem::path FACT_CACHE_DIR = "/tmp/cats-cache";

static std::string LocalDate(std::time_t now) {
  std::tm local;
  localtime_r(&now, &local);
  char day[11];
  std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
  return day;
}

static std::chrono::system_clock::time_point NextLocalMidnight(std::time_t now) {
  std::tm local;
  localtime_r(&now, &local);
  ++local.tm_mday;
  local.tm_hour = local.tm_min = local.tm_sec = 0;
  local.tm_isdst = -1;
  return std::chrono::system_clock::from_time_t(std::mktime(&local));
}

static bool IsValidUtf8Text(const std::string &text) {
  size_t position = 0;
  while (position < text.size()) {
    const auto first = static_cast<unsigned char>(text[position++]);
    // DrawText takes a null-terminated string.
    if (first == 0) return false;
    if (first < 0x80) continue;
    if (first < 0xc2 || first > 0xf4) return false;

    const size_t continuation_count = first < 0xe0 ? 1 : first < 0xf0 ? 2 : 3;
    if (text.size() - position < continuation_count) return false;
    const auto second = static_cast<unsigned char>(text[position]);
    // These bounds reject overlong forms, surrogates, and values above U+10FFFF.
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second > 0x9f) ||
        (first == 0xf0 && second < 0x90) || (first == 0xf4 && second > 0x8f)) {
      return false;
    }
    for (size_t i = 0; i < continuation_count; ++i) {
      if ((static_cast<unsigned char>(text[position++]) & 0xc0) != 0x80) return false;
    }
  }
  return true;
}

static std::string CleanFactText(std::string text) {
  if (!IsValidUtf8Text(text)) {
    fprintf(stderr, "Daily fact contained malformed UTF-8 or an embedded NUL\n");
    return {};
  }
  std::replace(text.begin(), text.end(), '\n', ' ');
  std::replace(text.begin(), text.end(), '\r', ' ');
  for (size_t pos = text.find("  "); pos != std::string::npos; pos = text.find("  ", pos)) {
    text.erase(pos, 1);
  }
  text.erase(0, text.find_first_not_of(' '));
  if (!text.empty()) text.erase(text.find_last_not_of(' ') + 1);
  if (text.size() > 150) {
    size_t end = 147;
    while ((static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text = text.substr(0, end) + "...";
  }
  return text;
}

static DailyFact ReadFactJson(std::istream &input, const std::string &day) {
  try {
    Json::CharReaderBuilder builder;
    Json::Value root;
    if (!Json::parseFromStream(builder, input, &root, nullptr) || !root.isObject() ||
        !root["id"].isString() || !root["text"].isString()) {
      return {};
    }

    std::string id = root["id"].asString();
    if (id.empty() || !IsValidUtf8Text(id)) return {};
    std::string text = CleanFactText(root["text"].asString());
    return text.empty() ? DailyFact{} : DailyFact{day, std::move(id), std::move(text)};
  } catch (const Json::Exception &error) {
    fprintf(stderr, "Could not parse daily fact JSON: %s\n", error.what());
    return {};
  }
}

static DailyFact LoadLatestCachedFact(const std::string &today) {
  namespace fs = std::filesystem;
  DailyFact latest;
  std::error_code error;
  for (fs::directory_iterator entry(FACT_CACHE_DIR, error), end;
       !error && entry != end; entry.increment(error)) {
    const fs::path &path = entry->path();
    const std::string day = path.stem().string();
    const bool legacy = path.extension() == ".txt";
    if ((!legacy && path.extension() != ".json") ||
        day.size() != 10 || day[4] != '-' || day[7] != '-' ||
        day > today || day < latest.day || (legacy && day == latest.day)) {
      continue;
    }

    std::error_code status_error;
    if (!entry->is_regular_file(status_error)) continue;
    std::ifstream input(path);
    if (!input) continue;

    DailyFact fact;
    if (legacy) {
      std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
      if (text.rfind(FACT_PREFIX, 0) == 0) text.erase(0, sizeof(FACT_PREFIX) - 1);
      fact = {day, {}, CleanFactText(std::move(text))};
    } else {
      fact = ReadFactJson(input, day);
    }
    if (!fact.text.empty()) latest = std::move(fact);
  }
  return latest;
}

static void CacheFact(const DailyFact &fact) {
  namespace fs = std::filesystem;
  std::error_code error;
  fs::create_directories(FACT_CACHE_DIR, error);
  const fs::path path = FACT_CACHE_DIR / (fact.day + ".json");
  Json::Value value;
  value["id"] = fact.id;
  value["text"] = fact.text;
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  const std::string json = Json::writeString(builder, value);

  // Write to a separate file so another reader only sees complete entries.
  std::string temp_path = path.string() + ".XXXXXX";
  const int descriptor = error ? -1 : mkstemp(temp_path.data());
  if (descriptor != -1) {
    FILE *output = fdopen(descriptor, "w");
    if (output) {
      const bool written = fwrite(json.data(), 1, json.size(), output) == json.size();
      const bool closed = fclose(output) == 0;
      if (written && closed) {
        fs::rename(temp_path, path, error);
        if (!error) {
          printf("Cached daily fact for %s\n", fact.day.c_str());
          return;
        }
      }
    } else {
      close(descriptor);
    }
    fs::remove(temp_path, error);
  }
  fprintf(stderr, "Could not cache the daily fact for %s\n", fact.day.c_str());
}

static DailyFact FetchDailyFact(const std::string &day) {
  printf("Fetching the daily fact for %s\n", day.c_str());
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> request(curl_easy_init(), curl_easy_cleanup);
  if (!request) {
    fprintf(stderr, "Failed to initialize CURL\n");
    return {};
  }

  CURL *curl = request.get();
  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, "https://uselessfacts.jsph.pl/api/v2/facts/today");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "LED-Matrix-Facts/1.0");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, AbortFactRequest);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFactResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

  const CURLcode result = curl_easy_perform(curl);
  if (should_stop_fact_thread) return {};
  if (result != CURLE_OK) {
    fprintf(stderr, "Could not fetch the daily fact: %s\n", curl_easy_strerror(result));
    return {};
  }
  if (!IsValidUtf8Text(response)) {
    fprintf(stderr, "Daily fact response contained malformed UTF-8 or an embedded NUL\n");
    return {};
  }

  std::istringstream input(response);
  DailyFact fact = ReadFactJson(input, day);
  if (fact.text.empty()) fprintf(stderr, "The daily fact response had no valid ID and text.\n");
  return fact;
}

static void PublishFact(const DailyFact &fact) {
  {
    std::lock_guard<std::mutex> lock(fact_mutex);
    current_fact = fact.text;
  }
  WakeDisplay();
}

static void FactUpdateThread(std::string checked_day, DailyFact displayed) {
  using Clock = std::chrono::system_clock;
  constexpr int retry_minutes[] = {1, 5, 15, 60};
  size_t retry_index = 0;

  while (!should_stop_fact_thread) {
    const std::time_t now = std::time(nullptr);
    const std::string today = LocalDate(now);
    auto wake_at = NextLocalMidnight(now);

    if (today != checked_day) {
      checked_day = today;
      retry_index = 0;
      auto cached = LoadLatestCachedFact(today);
      if (!cached.text.empty() &&
          (cached.day > displayed.day || displayed.day > today)) {
        displayed = std::move(cached);
        PublishFact(displayed);
        printf("Loaded cached fact for %s\n", displayed.day.c_str());
      }
    }

    if (displayed.day != today) {
      DailyFact fresh = FetchDailyFact(today);
      if (should_stop_fact_thread) return;
      if (LocalDate(std::time(nullptr)) != today) continue;

      const bool matches_displayed = displayed.id.empty()
          ? fresh.text == displayed.text : fresh.id == displayed.id;
      if (!fresh.text.empty() && !matches_displayed) {
        displayed = std::move(fresh);
        PublishFact(displayed);
        CacheFact(displayed);
      } else {
        const int minutes = retry_minutes[retry_index];
        printf("Daily fact is unavailable or unchanged. Retrying in %d minute(s).\n", minutes);
        wake_at = std::min(wake_at, Clock::now() + std::chrono::minutes(minutes));
        retry_index = std::min(retry_index + 1, std::size(retry_minutes) - 1);
      }
    }

    std::unique_lock<std::mutex> lock(fact_stop_mutex);
    fact_stop_cv.wait_until(lock, wake_at, []{ return should_stop_fact_thread.load(); });
  }
}

// Thread-safe function to get current fact
std::string GetCurrentFact() {
  std::lock_guard<std::mutex> lock(fact_mutex);
  return current_fact;
}

// Pre-render a GraphicsMagick image to a flat pixel array.
// This is done once at startup so the render loop avoids the expensive
// per-pixel pixelColor() accessor from GraphicsMagick.
static PreRenderedImage PreRenderImage(const Magick::Image &image) {
  PreRenderedImage result;
  result.width = image.columns();
  result.height = image.rows();
  result.animation_delay = image.animationDelay();
  result.pixels.resize(result.width * result.height);
  const bool has_alpha = image.matte();
  for (int y = 0; y < result.height; ++y) {
    for (int x = 0; x < result.width; ++x) {
      const Magick::Color c = image.pixelColor(x, y);
      const int opacity = has_alpha ? 255 - ScaleQuantumToChar(c.alphaQuantum()) : 255;
      Pixel &p = result.pixels[y * result.width + x];
      p.r = (ScaleQuantumToChar(c.redQuantum()) * opacity + 127) / 255;
      p.g = (ScaleQuantumToChar(c.greenQuantum()) * opacity + 127) / 255;
      p.b = (ScaleQuantumToChar(c.blueQuantum()) * opacity + 127) / 255;
    }
  }
  return result;
}

static PreRenderedFrames LoadPortrait(const char *filename) {
  PreRenderedFrames result;
  try {
    ImageVector frames;
    readImages(&frames, filename);

    // Animated files can store partial frames. Assemble them before cropping.
    if (frames.size() > 1) {
      ImageVector coalesced;
      Magick::coalesceImages(&coalesced, frames.begin(), frames.end());
      frames.swap(coalesced);
    }

    Magick::Geometry size(PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
    size.aspect(true);
    result.reserve(frames.size());
    for (Magick::Image &frame : frames) {
      const size_t width = frame.columns();
      const size_t height = frame.rows();
      const size_t crop_width = std::min(width, std::max<size_t>(1, height * PORTRAIT_WIDTH / PORTRAIT_HEIGHT));
      const size_t crop_height = std::min(height, std::max<size_t>(1, width * PORTRAIT_HEIGHT / PORTRAIT_WIDTH));
      frame.crop(Magick::Geometry(crop_width, crop_height,
                                  (width - crop_width) / 2, (height - crop_height) / 2));
      frame.repage();
      frame.resize(size, Magick::LanczosFilter);
      result.push_back(PreRenderImage(frame));
    }
  } catch (const std::exception &e) {
    fprintf(stderr, "Error loading %s: %s\n", filename, e.what());
    return {};
  }
  printf("Loaded %s: %zu frame(s), cropped to %dx%d\n",
         filename, result.size(), PORTRAIT_WIDTH, PORTRAIT_HEIGHT);
  return result;
}

static void DrawPortrait(const PreRenderedImage &image, Canvas *canvas) {
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      const Pixel &p = image.pixels[y * image.width + x];
      canvas->SetPixel(PORTRAIT_X + x, PORTRAIT_Y + y, p.r, p.g, p.b);
    }
  }
}

static int BrightnessForLocalTime(const std::tm &local) {
  constexpr int day_start = 8 * 60;
  constexpr int night_start = 23 * 60 + 30;
  const int minutes = local.tm_hour * 60 + local.tm_min;
  return minutes < day_start || minutes >= night_start ? 10 : 100;
}

static void DrawCenteredText(Canvas *canvas, const Font &font, int baseline,
                             Color color, const char *text) {
  int width = 0;
  for (const char *letter = text; *letter; ++letter) {
    width += font.CharacterWidth(static_cast<uint8_t>(*letter));
  }
  const int x = CLOCK_X + (CLOCK_WIDTH - width) / 2;
  DrawText(canvas, font, x, baseline, color, text);
}

static void DrawClock(Canvas *canvas, const Font &clock_font, const Font &date_font,
                      const std::tm &local) {
  char time_text[16];
  char date_text[16];
  strftime(time_text, sizeof(time_text), "%H:%M", &local);
  strftime(date_text, sizeof(date_text), "%-m/%-d", &local);

  DrawCenteredText(canvas, clock_font, CLOCK_BASELINE, Color(255, 255, 255), time_text);
  DrawCenteredText(canvas, date_font, DATE_BASELINE, Color(180, 180, 180), date_text);
}

// Rasterize each fact once. A render step only visits pixels inside the panel.
class FactStrip final : public Canvas {
public:
  void SetText(const Font &font, const std::string &text) {
    // DrawText also measures text. Start beyond an empty canvas to measure
    // with the same UTF-8 decoding and glyph fallback used for rendering.
    width_ = 0;
    width_ = DrawText(this, font, 1, FACT_BASELINE - TOP, COLOR, text.c_str());
    first_ink_column_ = width_;
    pixels_.assign(width_ * height(), 0);
    DrawText(this, font, 0, FACT_BASELINE - TOP, COLOR, text.c_str());
  }

  void Draw(Canvas *canvas, int offset) const {
    offset -= first_ink_column_;
    const int left = std::max(first_ink_column_, -offset);
    const int right = std::min(width_, MATRIX_WIDTH - offset);
    for (int y = 0; y < height(); ++y) {
      for (int x = left; x < right; ++x) {
        if (pixels_[y * width_ + x]) {
          canvas->SetPixel(x + offset, y + TOP, COLOR.r, COLOR.g, COLOR.b);
        }
      }
    }
  }

  int width() const override { return width_; }
  int height() const override { return MATRIX_HEIGHT - TOP; }
  int content_width() const { return width_ - first_ink_column_; }

private:
  void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) override {
    if (x >= 0 && x < width_ && y >= 0 && y < height()) {
      pixels_[y * width_ + x] = r || g || b;
      if (r || g || b) first_ink_column_ = std::min(first_ink_column_, x);
    }
  }

  void Clear() override { std::fill(pixels_.begin(), pixels_.end(), 0); }
  void Fill(uint8_t r, uint8_t g, uint8_t b) override {
    std::fill(pixels_.begin(), pixels_.end(), r || g || b);
  }

  static constexpr int TOP = PORTRAIT_Y + PORTRAIT_HEIGHT;
  inline static const Color COLOR{100, 255, 100};
  int width_ = 0;
  int first_ink_column_ = 0;
  std::vector<uint8_t> pixels_;
};

static std::chrono::milliseconds AnimationDelay(const PreRenderedImage &image) {
  return std::chrono::milliseconds(image.animation_delay > 0 ? image.animation_delay * 10 : 100);
}

static bool ShowRotatingPortraits(const PreRenderedFrames &first, const PreRenderedFrames &second,
                                  CatsMatrix *matrix, const Font &clock_font, const Font &date_font,
                                  const Font &fact_font) {
  using WallClock = std::chrono::system_clock;
  CatsFrame *offscreen = matrix->CreateFrameCanvas();
  CatsFrame *background = matrix->CreateFrameCanvas();
  const PreRenderedFrames *portrait = &first;
  size_t frame_index = 0;
  const auto start = DisplayClock::now();
  auto rotate_at = start + PORTRAIT_DURATION;
  auto animate_at = start + AnimationDelay(portrait->front());
  auto scroll_at = start + SCROLL_PAUSE;
  FactStrip fact_strip;
  std::string fact_text;
  int scroll_position = 0;

  bool check_fact = true;
  time_t last_clock_minute = -1;
  int last_brightness = -1;

  while (!interrupt_received) {
    const auto wall_now = WallClock::now();
    const auto tick = DisplayClock::now();
    const auto wall_second = std::chrono::floor<std::chrono::seconds>(wall_now);
    const time_t now = WallClock::to_time_t(wall_second);
    const time_t minute = now / 60;
    const bool redraw_clock = minute != last_clock_minute;
    bool redraw_portrait = false;
    bool redraw_ticker = false;

    if (tick >= rotate_at) {
      portrait = portrait == &first ? &second : &first;
      frame_index = 0;
      rotate_at = tick + PORTRAIT_DURATION;
      animate_at = tick + AnimationDelay(portrait->front());
      redraw_portrait = true;
    } else if (portrait->size() > 1 && tick >= animate_at) {
      frame_index = (frame_index + 1) % portrait->size();
      animate_at = tick + AnimationDelay((*portrait)[frame_index]);
      redraw_portrait = true;
    }

    if (check_fact) {
      std::string latest_fact = GetCurrentFact();
      if (latest_fact != fact_text) {
        fact_text.swap(latest_fact);
        fact_strip.SetText(fact_font, fact_text);
        scroll_position = 0;
        scroll_at = tick + SCROLL_PAUSE;
        redraw_ticker = true;
      }
    }

    const int fact_width = fact_strip.content_width();
    const bool scroll = fact_width > MATRIX_WIDTH - PORTRAIT_X;
    const int scroll_period = fact_width + SCROLL_GAP;
    if (scroll && tick >= scroll_at) {
      scroll_position = (scroll_position + 1) % scroll_period;
      scroll_at = tick + SCROLL_STEP;
      redraw_ticker = true;
    }

    if (redraw_clock) {
      std::tm local;
      localtime_r(&now, &local);
      const int brightness = BrightnessForLocalTime(local);
      if (brightness != last_brightness) {
        matrix->SetBrightness(brightness);
        last_brightness = brightness;
      }
      background->Clear();
      DrawClock(background, clock_font, date_font, local);
      last_clock_minute = minute;
      redraw_portrait = true;
    }
    if (redraw_portrait) DrawPortrait((*portrait)[frame_index], background);
    if (redraw_portrait || redraw_ticker) {
      offscreen->CopyFrom(*background);
      const int ticker_x = PORTRAIT_X - scroll_position;
      fact_strip.Draw(offscreen, ticker_x);
      // The next copy can enter before the previous copy finishes leaving.
      if (scroll) fact_strip.Draw(offscreen, ticker_x + scroll_period);
      offscreen = matrix->SwapOnVSync(offscreen);
      if (!offscreen) {
        fprintf(stderr, "Could not submit a display frame\n");
        return false;
      }
    }
#ifdef CATS_PREVIEW
    const auto window_status = matrix->PollEvents();
    if (window_status != CatsMatrix::WindowStatus::Open) {
      return window_status == CatsMatrix::WindowStatus::Closed;
    }
#endif

    const auto next_minute = std::chrono::floor<std::chrono::minutes>(wall_now) +
                             std::chrono::minutes(1);
    const auto wall_delay = std::chrono::ceil<DisplayClock::duration>(next_minute - wall_now);
    auto wake_at = std::min(rotate_at, tick + wall_delay);
    if (scroll) wake_at = std::min(wake_at, scroll_at);
    if (portrait->size() > 1) wake_at = std::min(wake_at, animate_at);
#ifdef CATS_PREVIEW
    wake_at = std::min(wake_at, tick + SCROLL_STEP);
#endif
    const DisplayWake wake = WaitForDisplayUpdate(wake_at);
    if (wake == DisplayWake::Error) return false;
    check_fact = wake == DisplayWake::Notification;
  }
  return true;
}

int usage(const char *progname) {
#ifdef CATS_PREVIEW
  fprintf(stderr, "Usage: %s <first-image> <second-image>\n", progname);
#else
  fprintf(stderr, "Usage: %s [led-matrix-options] <first-image> <second-image>\n", progname);
#endif
  fprintf(stderr, "\nAlternates two %dx%d portraits every %lld seconds on a 64x32 matrix.\n",
          PORTRAIT_WIDTH, PORTRAIT_HEIGHT, static_cast<long long>(PORTRAIT_DURATION.count()));
  fprintf(stderr, "The clock and date sit on the right and the daily fact scrolls below.\n\n");
#ifndef CATS_PREVIEW
  rgb_matrix::PrintMatrixFlags(stderr, DefaultMatrixOptions());
#endif
  return 1;
}

int main(int argc, char *argv[]) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Magick::InitializeMagick(*argv);

#ifndef CATS_PREVIEW
  auto matrix_options = DefaultMatrixOptions();
  rgb_matrix::RuntimeOptions runtime_opt;
  if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv,
                                         &matrix_options, &runtime_opt)) {
    return usage(argv[0]);
  }
#endif

  if (argc < 3) {
    return usage(argv[0]);
  }

  const char *first_filename = argv[1];
  const char *second_filename = argv[2];

  if (!InitializeDisplayWakeup()) {
    perror("Could not initialize display wakeup pipe");
    return 1;
  }
  signal(SIGTERM, InterruptHandler);
  signal(SIGINT, InterruptHandler);

#ifdef CATS_PREVIEW
  auto matrix = CatsMatrix::Create(MATRIX_WIDTH, MATRIX_HEIGHT);
#else
  std::unique_ptr<CatsMatrix> matrix(CatsMatrix::CreateFromOptions(matrix_options, runtime_opt));
#endif
  if (!matrix) {
    fprintf(stderr, "Failed to create matrix\n");
    return 1;
  }

  Font date_font;
  Font clock_font;
  Font fact_font;
  if (!date_font.LoadFont(CATS_FONT_DIR "/5x7.bdf")) {
    fprintf(stderr, "Could not load 5x7.bdf for date.\n");
    return 1;
  }
  const Font &clock = clock_font.LoadFont(CATS_FONT_DIR "/6x13.bdf") ? clock_font : date_font;
  const Font &ticker = fact_font.LoadFont(CATS_FONT_DIR "/5x8.bdf") ? fact_font : date_font;

  // Verify matrix dimensions
  if (matrix->width() != MATRIX_WIDTH || matrix->height() != MATRIX_HEIGHT) {
    fprintf(stderr, "Warning: Expected %dx%d matrix, got %dx%d\n",
            MATRIX_WIDTH, MATRIX_HEIGHT, matrix->width(), matrix->height());
  }

  const PreRenderedFrames first = LoadPortrait(first_filename);
  if (first.empty()) {
    fprintf(stderr, "Failed to load first image: %s\n", first_filename);
    return 1;
  }

  const PreRenderedFrames second = LoadPortrait(second_filename);
  if (second.empty()) {
    fprintf(stderr, "Failed to load second image: %s\n", second_filename);
    return 1;
  }

  const std::string today = LocalDate(std::time(nullptr));
  DailyFact cached = LoadLatestCachedFact(today);
  if (!cached.text.empty()) {
    current_fact = cached.text;
    printf("Loaded cached fact for %s\n", cached.day.c_str());
  }
  std::thread fact_thread(FactUpdateThread, today, std::move(cached));
  const bool display_ok = ShowRotatingPortraits(first, second, matrix.get(), clock, date_font, ticker);

  // Clean shutdown - wake the fact thread so it exits promptly
  printf("Shutting down...\n");
  {
    std::lock_guard<std::mutex> lock(fact_stop_mutex);
    should_stop_fact_thread = true;
  }
  fact_stop_cv.notify_all();
  fact_thread.join();

  matrix.reset();
  curl_global_cleanup();
  return display_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
