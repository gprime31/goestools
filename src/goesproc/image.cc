#include "image.h"

#include <cassert>

std::unique_ptr<Image> Image::createFromFile(
    std::shared_ptr<const lrit::File> f) {
  auto ish = f->getHeader<lrit::ImageStructureHeader>();
  auto ifs = f->getData();
  cv::Mat raw(ish.lines, ish.columns, CV_8UC1);
  if (ish.bitsPerPixel == 1) {
    // Number of pixels
    unsigned long n = (raw.size().width * raw.size().height);
    assert(ph.dataLength == n);

    // Read everything in one shot
    // Round up to nearest multiple of 8 because we're reading bytes
    auto line = std::make_unique<char[]>((n + 7) / 8);
    ifs->read(line.get(), (n + 7) / 8);
    assert(*ifs);

    // Pixel by pixel
    for (unsigned long i = 0; i < n; i += 8) {
      auto byte = line.get()[i / 8];
      for (auto j = i; j < (i + 8); j++) {
        if (byte & 0x80) {
          ((char*)raw.data)[j] = (char)0xff;
        } else {
          ((char*)raw.data)[j] = (char)0x00;
        }
        byte <<= 1;
      }
    }
  } else if (ish.bitsPerPixel == 8) {
    ifs->read((char*)raw.data, raw.size().width * raw.size().height);
    assert(ifs);
  } else {
    std::cerr << "bitsPerPixel == " << ish.bitsPerPixel << std::endl;
    assert(false);
  }

  return std::make_unique<Image>(raw, Area());
}

std::unique_ptr<Image> Image::createFromFiles(
    std::vector<std::shared_ptr<const lrit::File> > fs) {
  auto f = fs.front();

  // Sort images by their segment number
  std::sort(
    fs.begin(),
    fs.end(),
    [](const auto& a, const auto& b) -> bool {
      auto sa = a->template getHeader<lrit::SegmentIdentificationHeader>();
      auto sb = b->template getHeader<lrit::SegmentIdentificationHeader>();
      return sa.segmentNumber < sb.segmentNumber;
    });

  // NOAA LRIT header is identical across segments
  auto nl = fs.front()->getHeader<lrit::NOAALRITHeader>();

  // Detect if this image uses "new style" Himawari-8 headers.
  // If so, the line offset property of the image navigation header is
  // no longer specific to a particular segment, but equal across
  // segments. We need to use the "start line of segment" property to
  // determine the offset of a segment.
  bool hw8new = false;
  if (nl.productID == 43) {
    uint32_t lineOffset = 0;
    uint32_t i = 0;
    for (i = 0; i < fs.size(); i++) {
      auto in = fs[i]->getHeader<lrit::ImageNavigationHeader>();
      if (i == 0) {
        lineOffset = in.lineOffset;
      } else {
        if (in.lineOffset != lineOffset) {
          break;
        }
      }
    }
    // If the loop didn't break then lineOffset is equal across segments
    hw8new = (i == fs.size());
  }

  // Compute geometry of area shown by this image
  Area area;
  for (unsigned i = 0; i < fs.size(); i++) {
    auto& f = fs[i];
    auto is = f->getHeader<lrit::ImageStructureHeader>();
    auto in = f->getHeader<lrit::ImageNavigationHeader>();
    auto si = f->getHeader<lrit::SegmentIdentificationHeader>();

    // Compute relative area for this segment
    Area tmp;
    tmp.minColumn = -in.columnOffset;
    tmp.maxColumn = -in.columnOffset + is.columns;
    tmp.minLine = -((int32_t) in.lineOffset);
    tmp.maxLine = -((int32_t) in.lineOffset) + is.lines;

    // For Himawari-8, the offset accounting may be done differently.
    if (nl.productID == 43 && hw8new) {
      auto lineOffset = -in.lineOffset + si.segmentStartLine;
      tmp.minLine = lineOffset;
      tmp.maxLine = lineOffset + is.lines;
    }

    // Update
    if (i == 0) {
      area = tmp;
    } else {
      area = area.getUnion(tmp);
    }
  }

  cv::Mat raw(area.height(), area.width(), CV_8UC1);
  for (const auto& f : fs) {
    auto ish = f->getHeader<lrit::ImageStructureHeader>();
    auto sih = f->getHeader<lrit::SegmentIdentificationHeader>();

    char* chunk = (char*) raw.data + (sih.segmentStartLine * area.width());
    auto ifs = f->getData();
    ifs->read(chunk, ish.lines * ish.columns);
    assert(*ifs);
  }

  auto image = std::make_unique<Image>(raw, area);
  if (f->hasHeader<lrit::ImageNavigationHeader>()) {
    auto in = f->getHeader<lrit::ImageNavigationHeader>();
    image->columnScaling_ = in.columnScaling;
    image->lineScaling_ = in.lineScaling;
  }

  return image;
}

std::unique_ptr<Image> Image::generateFalseColor(
    const std::unique_ptr<Image>& i0,
    const std::unique_ptr<Image>& i1,
    cv::Mat lut) {
  auto img0 = i0->getRawImage();
  auto img1 = i1->getRawImage();

  // Dimensions may not be equal if we're working with mesoscale
  // images (GOES-R series). Use the larger of the two images directly
  // and resize the smaller of the two to make their dimensions match.
  if (img0.size() != img1.size()) {
    auto s0 = img0.rows * img0.cols;
    auto s1 = img1.rows * img1.cols;
    if (s0 < s1) {
      cv::Mat tmp(img1.size(), CV_8UC1);
      cv::resize(img0, tmp, tmp.size());
      img0 = tmp;
    } else {
      cv::Mat tmp(img0.size(), CV_8UC1);
      cv::resize(img1, tmp, tmp.size());
      img1 = tmp;
    }
  }

  uint8_t* data0 = img0.data;
  uint8_t* data1 = img1.data;
  auto rows = img0.rows;
  auto cols = img0.cols;

  cv::Mat raw(rows, cols, CV_8UC3);
  uint8_t* ptr = (uint8_t*) raw.data;
  for (auto y = 0; y < rows; y++) {
    for (auto x = 0; x < cols; x++) {
      auto luty = data0[y * cols + x];
      auto lutx = data1[y * cols + x];
      ptr[(y * cols + x) * 3 + 0] = lut.data[(luty * 256 + lutx) * 3 + 0];
      ptr[(y * cols + x) * 3 + 1] = lut.data[(luty * 256 + lutx) * 3 + 1];
      ptr[(y * cols + x) * 3 + 2] = lut.data[(luty * 256 + lutx) * 3 + 2];
    }
  }

  return std::make_unique<Image>(raw, i0->area_);
}

Image::Image(cv::Mat m, const Area& area)
    : m_(m),
      area_(area),
      columnScaling_(1),
      lineScaling_(1) {
}

void Image::fillSides() {
  // Fill the contour with black pixels (left side)
  for (auto y = 0; y < m_.rows; y++) {
    uint8_t* data = (uint8_t*) m_.data + y * m_.cols;
    for (auto x = 0; x < m_.cols; x++) {
      if (data[x] == 0xff) {
        data[x] = 0x00;
      } else {
        break;
      }
    }
    for (auto x = m_.cols - 1; x >= 0; x--) {
      if (data[x] == 0xff) {
        data[x] = 0x00;
      } else {
        break;
      }
    }
  }
}

void Image::remap(const cv::Mat& img) {
  uint8_t* map = (uint8_t*) img.data;
  for (auto y = 0; y < m_.rows; y++) {
    uint8_t* data = (uint8_t*) m_.data + y * m_.cols;
    for (auto x = 0; x < m_.cols; x++) {
      data[x] = map[data[x]];
    }
  }
}

cv::Mat Image::getRawImage() const {
  return m_;
}

cv::Mat Image::getRawImage(const Area& roi) const {
  cv::Mat raw = getRawImage();
  int x = roi.minColumn - area_.minColumn;
  int y = roi.minLine - area_.minLine;
  int w = roi.maxColumn - roi.minColumn;
  int h = roi.maxLine - roi.minLine;
  assert(x >= 0);
  assert(y >= 0);
  assert(w > 0 && (w - x) <= area_.width());
  assert(h > 0 && (h - y) <= area_.height());
  cv::Rect crop(x, y, w, h);
  return raw(crop);
}

// Scale columns/lines per scaling factor in ImageNavigationHeader
cv::Size Image::scaleSize(cv::Size s, bool shrink) const {
  float f = ((float) columnScaling_ / (float) lineScaling_);
  if (f < 1.0) {
    if (shrink) {
      s.height = ceilf(s.height * f);
    } else {
      s.width = ceilf(s.width / f);
    }
  } else {
    if (shrink) {
      s.width = ceilf(s.width / f);
    } else {
      s.height = ceilf(s.height * f);
    }
  }
  return s;
}

cv::Mat Image::getScaledImage(bool shrink) const {
  cv::Mat raw = getRawImage();
  cv::Mat out(scaleSize(cv::Size(raw.cols, raw.rows), shrink), CV_8UC1);
  cv::resize(raw, out, out.size());
  return std::move(out);
}

cv::Mat Image::getScaledImage(const Area& roi, bool shrink) const {
  cv::Mat raw = getRawImage(roi);
  cv::Mat out(scaleSize(cv::Size(raw.cols, raw.rows), shrink), CV_8UC1);
  cv::resize(raw, out, out.size());
  return std::move(out);
}

void Image::save(const std::string& path) const {
  cv::imwrite(path, m_);
}
