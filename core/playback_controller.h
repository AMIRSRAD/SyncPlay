#pragma once
#include <string>

class PlaybackController {
public:
    virtual ~PlaybackController() = default;

    virtual bool loadFile(const std::string& path) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void seek(double time) = 0;
    virtual void setSpeed(double speed) = 0;
    virtual bool isPlaying() const = 0;
    virtual double currentTime() const = 0;
    virtual double duration() const = 0;
    virtual double speed() const = 0;
};
