#include "cats-preview.h"

#include <SDL.h>

#include <algorithm>
#include <cstdio>

PreviewMatrix::Frame::Frame(int width, int height)
    : width_(width), height_(height), pixels_(width * height * 3) {}

void PreviewMatrix::Frame::SetPixel(int x, int y, uint8_t red, uint8_t green, uint8_t blue) {
  if (x < 0 || x >= width_ || y < 0 || y >= height_) return;
  uint8_t *pixel = pixels_.data() + 3 * (y * width_ + x);
  pixel[0] = red;
  pixel[1] = green;
  pixel[2] = blue;
}

void PreviewMatrix::Frame::Clear() {
  std::fill(pixels_.begin(), pixels_.end(), 0);
}

void PreviewMatrix::Frame::Fill(uint8_t red, uint8_t green, uint8_t blue) {
  for (size_t i = 0; i < pixels_.size(); i += 3) {
    pixels_[i] = red;
    pixels_[i + 1] = green;
    pixels_[i + 2] = blue;
  }
}

void PreviewMatrix::Frame::CopyFrom(const Frame &other) {
  pixels_ = other.pixels_;
}

PreviewMatrix::PreviewMatrix(int width, int height)
    : width_(width), height_(height) {}

PreviewMatrix::~PreviewMatrix() {
  SDL_DestroyTexture(texture_);
  SDL_DestroyRenderer(renderer_);
  SDL_DestroyWindow(window_);
  SDL_Quit();
}

std::unique_ptr<PreviewMatrix> PreviewMatrix::Create(int width, int height) {
  std::unique_ptr<PreviewMatrix> matrix(new PreviewMatrix(width, height));
  if (!matrix->Initialize()) {
    std::fprintf(stderr, "Could not open LED preview: %s\n", SDL_GetError());
    return nullptr;
  }
  return matrix;
}

bool PreviewMatrix::Initialize() {
  constexpr int scale = 12;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  if (SDL_Init(SDL_INIT_VIDEO) < 0 ||
      SDL_CreateWindowAndRenderer(width_ * scale, height_ * scale,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI,
                                  &window_, &renderer_) < 0) {
    return false;
  }

  SDL_SetWindowTitle(window_, "Cats LED Matrix");
  SDL_SetWindowMinimumSize(window_, width_, height_);
  if (SDL_RenderSetLogicalSize(renderer_, width_, height_) < 0) return false;
  texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB24,
                               SDL_TEXTUREACCESS_STREAMING, width_, height_);
  front_ = CreateFrameCanvas();
  return texture_ != nullptr;
}

PreviewMatrix::Frame *PreviewMatrix::CreateFrameCanvas() {
  frames_.push_back(std::make_unique<Frame>(width_, height_));
  return frames_.back().get();
}

void PreviewMatrix::SetBrightness(int percent) {
  const uint8_t level = percent * 255 / 100;
  SDL_SetTextureColorMod(texture_, level, level, level);
}

PreviewMatrix::WindowStatus PreviewMatrix::PollEvents() {
  bool repaint = false;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT ||
        (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)) {
      return WindowStatus::Closed;
    }
    if (event.type == SDL_WINDOWEVENT &&
        (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
         event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
      repaint = true;
    }
  }
  return repaint && !PresentFrame(*front_) ? WindowStatus::Error : WindowStatus::Open;
}

bool PreviewMatrix::PresentFrame(const Frame &frame) {
  if (SDL_UpdateTexture(texture_, nullptr, frame.pixels_.data(), width_ * 3) < 0 ||
      SDL_RenderClear(renderer_) < 0 ||
      SDL_RenderCopy(renderer_, texture_, nullptr, nullptr) < 0) {
    std::fprintf(stderr, "Could not render LED preview: %s\n", SDL_GetError());
    return false;
  }
  SDL_RenderPresent(renderer_);
  return true;
}

PreviewMatrix::Frame *PreviewMatrix::SwapOnVSync(Frame *frame) {
  if (!PresentFrame(*frame)) return nullptr;
  Frame *previous = front_;
  front_ = frame;
  return previous;
}
