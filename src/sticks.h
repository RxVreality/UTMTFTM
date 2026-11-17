// The stick drift page's own logic (J4, 14 November; the page lives in the Decaf tab, J4's pick of two drafts). It runs the
// stick check, keeps what each controller's check found in drift.json, and hands the driver the numbers to fix each stick
// with (DriftChannel in live.h, maths in drift.h). No VR calls: main.cpp tells it which controller is in each hand, and the
// tests hand it made-up ones, so the whole check can be run offline.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "drift.h"
#include "live.h"
#include "ui.h"

namespace sticks {

// What a check found for one controller, by its serial number. Kept until it's checked again.
struct Saved {
  drift::Params p;           // what the driver gets, with on: it drifts
  float checkedX = 0, checkedY = 0;  // where the check found it resting: learning while you play stays near here
  bool drifts = false, touch = false;
  std::string verdict, when; // "Drifts up and to the left", "2026-11-15"
  std::vector<std::string> lines;
};

class Drift {
 public:
  explicit Drift(std::string dataDir);  // reads drift.json from there, if it's there

  // Every loop. dc: the driver's channel, or null with no shared file. serial: the controller in each hand, "" for none.
  // frame: that controller is a Steam Frame's, whose magnetic sticks don't wear this way. showing: the page is on screen.
  // now: seconds, any steady clock.
  void tick(fhlive::DriftChannel* dc, const std::string serial[2], const bool frame[2], bool showing, double now);
  // A tap on the page. True if it was one of the page's own.
  bool click(const std::string& id, double now);
  // Leaving the page part way through a check stops it, and nothing changes.
  void leave();
  fh::DriftModel model() const;  // everything but the header: game, live, hover and the like are main.cpp's
  bool hidden() const;           // every controller on is a Frame's: the Sticks switch on the Decaf page doesn't show
  bool ready() const { return ready_; }
  int step() const { return step_; }  // 0 the page, 1 to 3 the check's steps, 4 what it found

  // What just happened, for the line along the bottom. statusSerial moves each time there's something new.
  std::string status;
  int statusKind = 0;  // 0 plain, 1 good, 2 warning
  uint32_t statusSerial = 0;

  // For the tests.
  std::string file() const { return file_; }
  const std::map<std::string, Saved>& saved() const { return saved_; }
  bool fixOn() const { return on_; }

 private:
  struct Hand {
    std::string serial;
    bool frame = false;
    uint32_t found = 0;  // from the driver: 1 a stick, 2 it feels a thumb
    uint32_t read = 0;   // the next ring sample to read
    std::vector<drift::Sample> step;      // this step's samples
    std::vector<drift::Dot> trail;        // where it's been lately, for the picture
    double trailAt = 0;
    drift::Dot raw{0, 0}, out{0, 0};
    bool touched = false;
    drift::Rest rest;                     // what step 1 found
    std::vector<drift::Sample> edge;      // step 2's samples, kept for the analysis
    std::vector<float> reach;             // step 2, for the picture
    std::vector<drift::Landing> landed;   // step 3
    double restSeconds = 0;               // step 1: how long it's been left alone so far
    double lastT = -1;
    bool done = false;                    // this stick has finished this step
    drift::Findings found_;               // step 4
    bool touchBad = false;
  };
  void readRing(fhlive::DriftChannel* dc, int h, bool keep);
  void advance(double now);
  void finish();
  void publish(fhlive::DriftChannel* dc);
  void load();
  void save() const;
  void say(const std::string& s, int kind);
  bool present(int h) const { return (hands_[h].found & 1) != 0; }

  std::string file_;
  std::map<std::string, Saved> saved_;
  bool on_ = true;        // the Off / On switch: fix any stick that drifts
  Hand hands_[2];
  int step_ = 0;          // 0 the page, 1 leave them alone, 2 round the edge, 3 flick and let go, 4 what it found
  double stepStart_ = 0;
  bool measuring_ = false;
  bool ready_ = false;
  std::string published_[2];  // what was last handed to the driver for each hand, as text, so it's only sent on a change
  bool republish_ = true;
  double learnedAt_ = 0;
};

}  // namespace sticks
