#include "homebrew/trailer_player.hpp"

#include "homebrew/browse.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

namespace playden::homebrew {
constexpr std::size_t kMaxTrailerBytes = 8 * 1024 * 1024;
}

@interface PlaydenTrailerFetch : NSObject <NSURLSessionDataDelegate>
@property(nonatomic, strong) NSMutableData* body;
@property(nonatomic, strong) dispatch_semaphore_t done;
@property(nonatomic, assign) BOOL tooBig;
@property(nonatomic, assign) BOOL rejected;
@property(nonatomic, assign) NSInteger status;
@end

@implementation PlaydenTrailerFetch
- (instancetype)init {
  self = [super init];
  if (self) {
    _body = [NSMutableData data];
    _done = dispatch_semaphore_create(0);
    _status = 0;
  }
  return self;
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
  (void)session;
  (void)task;
  if ([response isKindOfClass:[NSHTTPURLResponse class]])
    self.status = [(NSHTTPURLResponse*)response statusCode];
  const long long length = response.expectedContentLength;
  if (self.status != 200 || (length > 0 && static_cast<std::size_t>(length) > playden::homebrew::kMaxTrailerBytes)) {
    self.tooBig = length > static_cast<long long>(playden::homebrew::kMaxTrailerBytes);
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
                 task:(NSURLSessionTask*)task
    willPerformHTTPRedirection:(NSHTTPURLResponse*)response
                    newRequest:(NSURLRequest*)request
             completionHandler:(void (^)(NSURLRequest*))completionHandler {
  (void)session;
  (void)task;
  (void)response;
  const char* next = request.URL.absoluteString.UTF8String;
  if (!next || !playden::homebrew::SteamTrailerUrl(next)) {
    self.rejected = YES;
    completionHandler(nil);
    return;
  }
  completionHandler(request);
}

- (void)URLSession:(NSURLSession*)session dataTask:(NSURLSessionDataTask*)task didReceiveData:(NSData*)data {
  (void)session;
  (void)task;
  if (self.body.length + data.length > playden::homebrew::kMaxTrailerBytes) {
    self.tooBig = YES;
    [task cancel];
    return;
  }
  [self.body appendData:data];
}

- (void)URLSession:(NSURLSession*)session task:(NSURLSessionTask*)task didCompleteWithError:(NSError*)error {
  (void)session;
  (void)task;
  (void)error;
  dispatch_semaphore_signal(self.done);
}
@end

namespace playden::homebrew {
namespace {

constexpr std::uint32_t kMaxFrameWidth = 1040;
constexpr std::uint32_t kMaxFrameHeight = 585;

}  // namespace

struct TrailerPlayer::Impl {
  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::thread worker;
  std::mutex frame_mu;
  std::mutex task_mu;
  std::vector<std::uint8_t> rgba;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t serial = 0;
  std::string url;
  std::string file;
  NSURLSession* session = nil;
  NSURLSessionDataTask* task = nil;

  void CancelDownload() {
    std::lock_guard<std::mutex> lock(task_mu);
    if (task) [task cancel];
    if (session) [session invalidateAndCancel];
  }

  void Publish(std::vector<std::uint8_t> pixels, std::uint32_t w, std::uint32_t h) {
    std::lock_guard<std::mutex> lock(frame_mu);
    rgba = std::move(pixels);
    width = w;
    height = h;
    ++serial;
  }
};

namespace {

std::vector<std::uint8_t> ScaleFrame(const std::uint8_t* bgra, std::uint32_t width, std::uint32_t height,
                                     std::size_t stride, std::uint32_t& out_w, std::uint32_t& out_h) {
  float scale = 1.0F;
  if (width > kMaxFrameWidth) scale = std::min(scale, static_cast<float>(kMaxFrameWidth) / static_cast<float>(width));
  if (height > kMaxFrameHeight)
    scale = std::min(scale, static_cast<float>(kMaxFrameHeight) / static_cast<float>(height));
  out_w = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(width) * scale));
  out_h = std::max(1u, static_cast<std::uint32_t>(static_cast<float>(height) * scale));
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(out_w) * out_h * 4u);
  for (std::uint32_t y = 0; y < out_h; ++y) {
    const std::uint32_t sy = std::min(height - 1, static_cast<std::uint32_t>(static_cast<float>(y) / scale));
    const std::uint8_t* row = bgra + static_cast<std::size_t>(sy) * stride;
    for (std::uint32_t x = 0; x < out_w; ++x) {
      const std::uint32_t sx = std::min(width - 1, static_cast<std::uint32_t>(static_cast<float>(x) / scale));
      const std::uint8_t* pixel = row + static_cast<std::size_t>(sx) * 4u;
      std::uint8_t* dest = rgba.data() + (static_cast<std::size_t>(y) * out_w + x) * 4u;
      dest[0] = pixel[2];
      dest[1] = pixel[1];
      dest[2] = pixel[0];
      dest[3] = pixel[3];
    }
  }
  return rgba;
}

bool WriteTrailerFile(const std::uint8_t* bytes, std::size_t size, std::string& path) {
  if (size < 12 || std::memcmp(bytes + 4, "ftyp", 4) != 0) return false;
  char pattern[] = "/tmp/playden-trailer-XXXXXX";
  const int fd = ::mkstemp(pattern);
  if (fd < 0) return false;
  std::string tmp = pattern;
  const std::string mp4 = tmp + ".mp4";
  ::close(fd);
  ::unlink(tmp.c_str());
  FILE* file = std::fopen(mp4.c_str(), "wb");
  if (!file) return false;
  const bool wrote = std::fwrite(bytes, 1, size, file) == size;
  std::fclose(file);
  if (!wrote) {
    ::unlink(mp4.c_str());
    return false;
  }
  path = mp4;
  return true;
}

void DecodeFile(std::atomic<bool>& stop, std::atomic<bool>& failed, const std::string& path,
                const std::function<void(std::vector<std::uint8_t>, std::uint32_t, std::uint32_t)>& publish) {
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSArray<AVAssetTrack*>* tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
    AVAssetTrack* track = tracks.firstObject;
    if (!track) {
      failed = true;
      return;
    }
    while (!stop.load(std::memory_order_acquire)) {
      NSError* error = nil;
      AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
      NSDictionary* settings = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      };
      AVAssetReaderTrackOutput* output = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
      output.alwaysCopiesSampleData = NO;
      if (error || !reader || ![reader canAddOutput:output]) {
        failed = true;
        return;
      }
      [reader addOutput:output];
      if (![reader startReading]) {
        failed = true;
        return;
      }
      const CFAbsoluteTime loop = CFAbsoluteTimeGetCurrent();
      bool any = false;
      while (!stop.load(std::memory_order_acquire) && reader.status == AVAssetReaderStatusReading) {
        CMSampleBufferRef sample = [output copyNextSampleBuffer];
        if (!sample) break;
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample);
        if (image && !CVPixelBufferIsPlanar(image)) {
          CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
          const auto width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(image));
          const auto height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(image));
          const auto* base = static_cast<const std::uint8_t*>(CVPixelBufferGetBaseAddress(image));
          const std::size_t stride = CVPixelBufferGetBytesPerRow(image);
          if (base && width > 0 && height > 0) {
            std::uint32_t out_w = 0;
            std::uint32_t out_h = 0;
            publish(ScaleFrame(base, width, height, stride, out_w, out_h), out_w, out_h);
            any = true;
          }
          CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
        }
        const Float64 pts = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
        CFRelease(sample);
        if (pts >= 0 && !stop.load(std::memory_order_acquire)) {
          const CFAbsoluteTime due = loop + pts;
          const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
          if (due > now) {
            const auto wait = static_cast<useconds_t>(std::min(1.0, due - now) * 1000000.0);
            ::usleep(wait);
          }
        }
      }
      [reader cancelReading];
      if (!any) {
        failed = true;
        return;
      }
      if (stop.load(std::memory_order_acquire)) return;
    }
  }
}

}  // namespace

TrailerPlayer::TrailerPlayer() : impl_(std::make_unique<Impl>()) {}

TrailerPlayer::~TrailerPlayer() { Stop(); }

void TrailerPlayer::Play(const std::string& url) {
  const std::string playable = PlayableTrailerUrl(url);
  if (impl_->worker.joinable() && impl_->url == playable && !impl_->failed.load(std::memory_order_acquire)) return;
  Stop();
  if (playable.empty()) {
    impl_->failed = true;
    std::fprintf(stderr, "artwork trailer rejected\n");
    return;
  }
  impl_->stop = false;
  impl_->failed = false;
  impl_->url = playable;
  impl_->worker = std::thread([impl = impl_.get(), url = playable] {
    @autoreleasepool {
      PlaydenTrailerFetch* fetch = [[PlaydenTrailerFetch alloc] init];
      NSURL* target = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
      if (!target) {
        impl->failed = true;
        std::fprintf(stderr, "artwork trailer bad-url\n");
        return;
      }
      NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
      config.timeoutIntervalForRequest = 15;
      config.timeoutIntervalForResource = 20;
      NSURLSession* session = [NSURLSession sessionWithConfiguration:config delegate:fetch delegateQueue:nil];
      NSURLSessionDataTask* task = [session dataTaskWithURL:target];
      {
        std::lock_guard<std::mutex> lock(impl->task_mu);
        impl->session = session;
        impl->task = task;
      }
      [task resume];
      dispatch_semaphore_wait(fetch.done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC));
      {
        std::lock_guard<std::mutex> lock(impl->task_mu);
        impl->task = nil;
        impl->session = nil;
      }
      [session invalidateAndCancel];
      if (impl->stop.load(std::memory_order_acquire)) return;
      if (fetch.rejected || fetch.tooBig || fetch.status != 200 || fetch.body.length < 12) {
        impl->failed = true;
        std::fprintf(stderr, "artwork trailer http=%ld bytes=%lu\n", static_cast<long>(fetch.status),
                     static_cast<unsigned long>(fetch.body.length));
        return;
      }
      std::string path;
      if (!WriteTrailerFile(static_cast<const std::uint8_t*>(fetch.body.bytes), fetch.body.length, path)) {
        impl->failed = true;
        std::fprintf(stderr, "artwork trailer not-mp4\n");
        return;
      }
      impl->file = path;
      std::fprintf(stderr, "artwork trailer play bytes=%lu\n", static_cast<unsigned long>(fetch.body.length));
      DecodeFile(impl->stop, impl->failed, path, [impl](std::vector<std::uint8_t> pixels, std::uint32_t w, std::uint32_t h) {
        impl->Publish(std::move(pixels), w, h);
      });
      ::unlink(path.c_str());
      impl->file.clear();
    }
  });
}

bool TrailerPlayer::running() const noexcept {
  return impl_->worker.joinable() && !impl_->failed.load(std::memory_order_acquire);
}

void TrailerPlayer::Stop() {
  impl_->stop = true;
  impl_->CancelDownload();
  if (impl_->worker.joinable()) impl_->worker.join();
  if (!impl_->file.empty()) {
    ::unlink(impl_->file.c_str());
    impl_->file.clear();
  }
  impl_->url.clear();
  impl_->failed = false;
  std::lock_guard<std::mutex> lock(impl_->frame_mu);
  impl_->rgba.clear();
  impl_->width = 0;
  impl_->height = 0;
}

bool TrailerPlayer::failed() const noexcept { return impl_->failed.load(std::memory_order_acquire); }

bool TrailerPlayer::TakeFrame(std::vector<std::uint8_t>& rgba, std::uint32_t& width, std::uint32_t& height,
                              std::uint64_t& serial) {
  std::lock_guard<std::mutex> lock(impl_->frame_mu);
  if (impl_->serial == 0 || impl_->serial == serial || impl_->rgba.empty()) return false;
  rgba = impl_->rgba;
  width = impl_->width;
  height = impl_->height;
  serial = impl_->serial;
  return true;
}

}  // namespace playden::homebrew
