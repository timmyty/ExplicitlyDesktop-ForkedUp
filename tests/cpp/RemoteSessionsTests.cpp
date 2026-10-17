/*
  ==============================================================================

    RemoteSessionsTests.cpp

    The broker between the phone (StreamHost) and the extension (the /remote endpoints).

  ==============================================================================
*/

#include "TestFramework.h"
#include "RemoteSessions.h"

#include <thread>

namespace
{
    /** Makes the extension look present (a poll that times out at once). */
    void pollOnce (RemoteSessions& rs)
    {
        RemoteSessions::Message m;
        rs.waitForMessage (m, 0);
    }
}

TEST (remote_offer_needs_a_polling_extension)
{
    RemoteSessions rs;
    CHECK (! rs.isExtensionAvailable());

    std::string id, answer, error;
    CHECK (rs.submitOffer ("dev", "Phone", "offer-sdp", id, answer, error, 100) == RemoteSessions::OfferResult::unavailable);

    pollOnce (rs);
    CHECK (rs.isExtensionAvailable());
}

TEST (remote_offer_answer_round_trip)
{
    RemoteSessions rs;
    pollOnce (rs);

    std::thread extension ([&rs]
    {
        RemoteSessions::Message m;
        if (rs.waitForMessage (m, 2000) && m.type == "offer" && m.sdp == "offer-sdp" && m.deviceName == "Phone")
            rs.deliverAnswer (m.sessionId, "answer-sdp", "");
    });

    std::string id, answer, error;
    auto result = rs.submitOffer ("dev", "Phone", "offer-sdp", id, answer, error, 2000);
    extension.join();

    CHECK (result == RemoteSessions::OfferResult::answered);
    CHECK_EQ (answer, std::string ("answer-sdp"));
    CHECK (id.rfind ("s-", 0) == 0);
    CHECK_EQ (rs.getCurrentDeviceId(), std::string ("dev"));

    // The extension reports the session running
    rs.setState (true, id, "Phone");
    CHECK (rs.getState().active);
    CHECK_EQ (rs.getState().deviceName, std::string ("Phone"));

    // Wrong device can't hang it up; the right one can
    CHECK (! rs.hangup ("other", id));
    CHECK (rs.hangup ("dev", id));
    CHECK (! rs.getState().active);

    RemoteSessions::Message m;
    CHECK (rs.waitForMessage (m, 0));
    CHECK_EQ (m.type, std::string ("hangup"));
    CHECK_EQ (m.sessionId, id);
    CHECK_EQ (m.reason, std::string ("hangup"));
}

TEST (remote_offer_timeout_queues_hangup)
{
    RemoteSessions rs;
    pollOnce (rs);

    std::string id, answer, error;
    CHECK (rs.submitOffer ("dev", "Phone", "sdp", id, answer, error, 50) == RemoteSessions::OfferResult::timedOut);

    RemoteSessions::Message m;
    CHECK (rs.waitForMessage (m, 0) && m.type == "offer");
    CHECK (rs.waitForMessage (m, 0) && m.type == "hangup" && m.sessionId == id && m.reason == "timeout");
    CHECK (! rs.deliverAnswer (id, "late", ""));   // Too late
}

TEST (remote_new_offer_replaces_old_session)
{
    RemoteSessions rs;
    pollOnce (rs);

    // First session, answered
    std::string id1, a1, e1;
    std::thread answer1 ([&rs]
    {
        RemoteSessions::Message m;
        if (rs.waitForMessage (m, 2000))
            rs.deliverAnswer (m.sessionId, "a1", "");
    });
    CHECK (rs.submitOffer ("dev1", "Phone 1", "o1", id1, a1, e1, 2000) == RemoteSessions::OfferResult::answered);
    answer1.join();
    rs.setState (true, id1, "Phone 1");

    // Second device: first gets a "replaced" hangup before the new offer
    std::string id2, a2, e2;
    std::thread answer2 ([&rs, &id1]
    {
        RemoteSessions::Message m;
        bool sawHangup = rs.waitForMessage (m, 2000) && m.type == "hangup" && m.sessionId == id1 && m.reason == "replaced";
        if (sawHangup && rs.waitForMessage (m, 2000) && m.type == "offer")
            rs.deliverAnswer (m.sessionId, "a2", "");
    });
    CHECK (rs.submitOffer ("dev2", "Phone 2", "o2", id2, a2, e2, 2000) == RemoteSessions::OfferResult::answered);
    answer2.join();
    CHECK_EQ (a2, std::string ("a2"));
    CHECK (id1 != id2);

    // A late "active" from the old session is ignored
    rs.setState (true, id1, "Phone 1");
    CHECK (! rs.getState().active);
    rs.setState (true, id2, "Phone 2");
    CHECK (rs.getState().active);

    // Revoke ends it
    rs.endSessionsForDevice ("dev2", "revoked");
    CHECK (! rs.getState().active);
    RemoteSessions::Message m;
    CHECK (rs.waitForMessage (m, 0) && m.type == "hangup" && m.reason == "revoked");
}

TEST (remote_extension_error_and_shutdown)
{
    RemoteSessions rs;
    pollOnce (rs);

    std::thread extension ([&rs]
    {
        RemoteSessions::Message m;
        if (rs.waitForMessage (m, 2000))
            rs.deliverAnswer (m.sessionId, "", "protected");
    });
    std::string id, answer, error;
    CHECK (rs.submitOffer ("dev", "Phone", "o", id, answer, error, 2000) == RemoteSessions::OfferResult::failed);
    extension.join();
    CHECK_EQ (error, std::string ("protected"));

    // Shutdown wakes a long poll
    std::thread poller ([&rs]
    {
        RemoteSessions::Message m;
        rs.waitForMessage (m, 20000);
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    auto t0 = std::chrono::steady_clock::now();
    rs.shutdown();
    poller.join();
    CHECK (std::chrono::steady_clock::now() - t0 < std::chrono::seconds (2));
}

TEST (remote_poll_cancel_flag)
{
    RemoteSessions rs;
    std::atomic<bool> cancel { false };
    std::thread poller ([&]
    {
        RemoteSessions::Message m;
        rs.waitForMessage (m, 20000, &cancel);
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    CHECK (rs.isExtensionAvailable());   // A poll is waiting
    auto t0 = std::chrono::steady_clock::now();
    cancel = true;
    poller.join();
    CHECK (std::chrono::steady_clock::now() - t0 < std::chrono::seconds (1));
}
