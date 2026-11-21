#ifndef CATS_PREVIEW_H
#define CATS_PREVIEW_H

#include "canvas.h"

#include <memory>
#include <vector>

struct SDL_Renderer;
struct SDL_Texture;
struct SDL_Window;

class PreviewMatrix {
public:
  class Frame final : public rgb_matrix::Canvas {
  public:
    Frame(int width, int height);
    int width() const override { return width_; }
    int height() const override { return height_; }
    void SetPixel(int x, int y, uint8_t red, uint8_t green, uint8_t blue) override;
    void Clear() override;
    void Fill(uint8_t red, uint8_t green, uint8_t blue) override;
    void CopyFrom(const Frame &other);

  private:
    friend class PreviewMatrix;

    const int width_;
    const int height_;
    std::vector<uint8_t> pixels_;
  };

  static std::unique_ptr<PreviewMatrix> Create(int width, int height);
  ~PreviewMatrix();

  enum class WindowStatus { Open, Closed, Error };

  int width() const { return width_; }
  int height() const { return height_; }
  Frame *CreateFrameCanvas();
  Frame *SwapOnVSync(Frame *frame);
  WindowStatus PollEvents();
  void SetBrightness(int percent);

private:
  PreviewMatrix(int width, int height);
  bool Initialize();
  bool PresentFrame(const Frame &frame);

  const int width_;
  const int height_;
  std::vector<std::unique_ptr<Frame>> frames_;
  Frame *front_ = nullptr;
  SDL_Window *window_ = nullptr;
  SDL_Renderer *renderer_ = nullptr;
  SDL_Texture *texture_ = nullptr;
};

#endif  // CATS_PREVIEW_H
