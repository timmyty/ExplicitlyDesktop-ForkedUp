/*
  ==============================================================================

    PairingQrView.h

    "Pair a phone" window content: the QR code for
    http://<lan-ip>:8767/#pair=<code>&host=<hostId>, the same code to type,
    and a countdown. Closes itself when the code expires.

  ==============================================================================
*/

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "qrcodegen.hpp"

class PairingQrView : public juce::Component,
                      private juce::Timer
{
public:
    PairingQrView (const juce::String& urlToShow, const juce::String& typedCode,
                   juce::Time expiryTime, std::function<void()> whenClosed)
        : url (urlToShow), code (typedCode), expires (expiryTime), onClosed (std::move (whenClosed)),
          qr (qrcodegen::QrCode::encodeText (urlToShow.toRawUTF8(), qrcodegen::QrCode::Ecc::MEDIUM))
    {
        setSize (440, 600);
        startTimer (500);
    }

    ~PairingQrView() override
    {
        stopTimer();
        if (onClosed)
            onClosed();
    }

    /** Closes the window this view is in (e.g. once the phone has used the code). */
    void close()
    {
        if (auto* window = findParentComponentOfClass<juce::DialogWindow>())
            window->exitModalState (0);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
        auto area = getLocalBounds().reduced (20);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (16.0f));
        g.drawFittedText ("Scan this with the phone's camera, on the same Wi-Fi as this PC.",
                          area.removeFromTop (40), juce::Justification::centred, 2);

        // QR code with its 4-module quiet zone on white
        int modules = qr.getSize() + 8;
        int side = juce::jmin (area.getWidth(), 360);
        int moduleSize = juce::jmax (1, side / modules);
        side = moduleSize * modules;
        auto qrArea = area.removeFromTop (side).withSizeKeepingCentre (side, side);

        g.setColour (juce::Colours::white);
        g.fillRect (qrArea);
        g.setColour (juce::Colours::black);
        for (int y = 0; y < qr.getSize(); ++y)
            for (int x = 0; x < qr.getSize(); ++x)
                if (qr.getModule (x, y))
                    g.fillRect (qrArea.getX() + (x + 4) * moduleSize, qrArea.getY() + (y + 4) * moduleSize,
                                moduleSize, moduleSize);

        area.removeFromTop (14);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText ("Or open the address below and type this code:", area.removeFromTop (22), juce::Justification::centred);

        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 22.0f, juce::Font::bold));
        g.drawText (code, area.removeFromTop (34), juce::Justification::centred);

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (juce::Colours::lightgrey);
        g.drawFittedText (url.upToFirstOccurrenceOf ("#", false, false), area.removeFromTop (22),
                          juce::Justification::centred, 1);

        auto secondsLeft = juce::jmax<juce::int64> (0, (expires.toMilliseconds() - juce::Time::currentTimeMillis()) / 1000);
        g.drawText ("Code expires in " + juce::String (secondsLeft) + " s (it works once)",
                    area.removeFromTop (22), juce::Justification::centred);
    }

private:
    void timerCallback() override
    {
        if (juce::Time::getCurrentTime() >= expires)
        {
            stopTimer();
            close();
            return;
        }
        repaint();
    }

    juce::String url, code;
    juce::Time expires;
    std::function<void()> onClosed;
    qrcodegen::QrCode qr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PairingQrView)
};
