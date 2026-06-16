#include "SyncComms/SegmentAnchoring.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace SyncComms {

namespace {

// Offsets within a match may only grow (cinematics/dead time add wall-clock
// the replay doesn't record); allow a little jitter below zero.
constexpr double kMinOffsetStep = -0.5;
// One cinematic + celebration + countdown never plausibly exceeds this.
constexpr double kMaxOffsetStep = 120.0;

bool StepsValid(const std::vector<double>& offsets) {
    for (size_t i = 1; i < offsets.size(); ++i) {
        double step = offsets[i] - offsets[i - 1];
        if (step < kMinOffsetStep || step > kMaxOffsetStep) return false;
    }
    return true;
}

// Fills offsets for segments without their own anchor: nearest preceding
// anchored segment's offset; segments before the first anchor inherit it.
void FillUnanchored(std::vector<double>& offsets, const std::vector<char>& has) {
    double first = 0.0;
    bool haveFirst = false;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (has[i]) { first = offsets[i]; haveFirst = true; break; }
    }
    if (!haveFirst) return;
    double prev = first;
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (has[i]) prev = offsets[i];
        else        offsets[i] = prev;
    }
}

} // namespace

const char* AnchorModeStr(AnchorMode mode) {
    switch (mode) {
        case AnchorMode::PerGoal:      return "per-goal";
        case AnchorMode::LegacyEnd:    return "legacy-end";
        case AnchorMode::SingleOffset: return "single-offset";
        case AnchorMode::None:         return "none";
    }
    return "none";
}

AnchorPlan ComputeAnchorPlan(const std::vector<SegmentInfo>& segments,
                             std::vector<int> goalFrames,
                             double frameTime) {
    AnchorPlan plan;
    const size_t n = segments.size();
    if (n == 0) {
        plan.note = "no segments";
        return plan;
    }
    if (goalFrames.empty()) {
        plan.note = "no goals in replay header — nothing to anchor against";
        return plan;
    }
    std::sort(goalFrames.begin(), goalFrames.end());

    std::vector<double> goalSec(goalFrames.size());
    for (size_t i = 0; i < goalFrames.size(); ++i) {
        goalSec[i] = goalFrames[i] * frameTime;
    }

    // Segments stamped with the exact goal moment, in chronological order.
    std::vector<size_t> stamped;
    for (size_t i = 0; i < n; ++i) {
        if (segments[i].goalTimeSec >= 0.0) stamped.push_back(i);
    }

    const bool legacy = stamped.empty();
    auto anchorOf = [&](size_t segIdx) {
        return legacy ? segments[segIdx].endTimeSec
                      : segments[segIdx].goalTimeSec;
    };

    // In legacy mode every segment is a candidate anchor except, possibly,
    // a trailing goal-less segment (match ended on the timer). Trim it when
    // the counts say it must exist.
    std::vector<size_t> anchors = stamped;
    if (legacy) {
        anchors.resize(n);
        for (size_t i = 0; i < n; ++i) anchors[i] = i;
        if (anchors.size() == goalSec.size() + 1) anchors.pop_back();
    }

    auto applyMatch = [&](size_t goalStart, AnchorMode mode, std::string note) {
        plan.mode = mode;
        plan.note = std::move(note);
        plan.offsets.assign(n, 0.0);
        std::vector<char> has(n, 0);
        plan.residuals.clear();
        for (size_t k = 0; k < anchors.size(); ++k) {
            size_t si = anchors[k];
            plan.offsets[si] = goalSec[goalStart + k] - anchorOf(si);
            has[si] = 1;
            plan.residuals.push_back(
                anchorOf(si) + plan.offsets[si] - goalSec[goalStart + k]);
        }
        FillUnanchored(plan.offsets, has);
    };

    auto applySingleOffset = [&](double offset, std::string note) {
        plan.mode = AnchorMode::SingleOffset;
        plan.note = std::move(note);
        plan.offsets.assign(n, offset);
        plan.residuals.clear();
        size_t pairs = std::min(anchors.size(), goalSec.size());
        for (size_t k = 0; k < pairs; ++k) {
            plan.residuals.push_back(anchorOf(anchors[k]) + offset - goalSec[k]);
        }
    };

    if (anchors.empty()) {
        plan.note = "no anchorable segments";
        return plan;
    }

    if (anchors.size() == goalSec.size()) {
        std::vector<double> candidate(anchors.size());
        for (size_t k = 0; k < anchors.size(); ++k) {
            candidate[k] = goalSec[k] - anchorOf(anchors[k]);
        }
        std::string note = legacy
            ? "legacy sidecar: anchored each segment end to its goal frame"
            : "matched each goal stamp to its goal frame";
        if (!StepsValid(candidate)) {
            note += " — WARNING: offset steps outside expected range "
                    "(clock skew or missed events?); kept in-order matching";
        }
        applyMatch(0, legacy ? AnchorMode::LegacyEnd : AnchorMode::PerGoal,
                   std::move(note));
        return plan;
    }

    if (anchors.size() < goalSec.size()) {
        // Capture covered a contiguous run of goals (started mid-match or
        // closed early). Find the window of goalFrames it fits.
        std::vector<size_t> validStarts;
        const size_t windows = goalSec.size() - anchors.size() + 1;
        for (size_t w = 0; w < windows; ++w) {
            std::vector<double> candidate(anchors.size());
            for (size_t k = 0; k < anchors.size(); ++k) {
                candidate[k] = goalSec[w + k] - anchorOf(anchors[k]);
            }
            if (StepsValid(candidate)) validStarts.push_back(w);
        }
        if (validStarts.size() == 1) {
            std::ostringstream note;
            note << "matched " << anchors.size() << " segments to goals ["
                 << validStarts[0] << ".." << (validStarts[0] + anchors.size() - 1)
                 << "] of " << goalSec.size();
            applyMatch(validStarts[0],
                       legacy ? AnchorMode::LegacyEnd : AnchorMode::PerGoal,
                       note.str());
            return plan;
        }
        std::ostringstream note;
        note << "ambiguous goal window (" << validStarts.size()
             << " candidates for " << anchors.size() << " segments / "
             << goalSec.size() << " goals) — single offset from last pair";
        applySingleOffset(goalSec.back() - anchorOf(anchors.back()), note.str());
        return plan;
    }

    // More anchorable segments than goals in the header. Every goal stamp
    // corresponds to a real goal the header must list, so this indicates a
    // header-parse problem (or, in legacy mode, >1 trailing goal-less
    // segment). Degrade to one offset from the first pair.
    std::ostringstream note;
    note << "more segments (" << anchors.size() << ") than goals ("
         << goalSec.size() << ") — header parse suspect; single offset "
            "from first pair";
    applySingleOffset(goalSec.front() - anchorOf(anchors.front()), note.str());
    return plan;
}

void ApplyAnchorPlan(std::vector<SegmentInfo>& segments, const AnchorPlan& plan) {
    if (plan.mode == AnchorMode::None) return;
    if (plan.offsets.size() != segments.size()) return;
    for (size_t i = 0; i < segments.size(); ++i) {
        segments[i].startTimeSec += plan.offsets[i];
        segments[i].endTimeSec   += plan.offsets[i];
        if (segments[i].goalTimeSec >= 0.0) {
            segments[i].goalTimeSec += plan.offsets[i];
        }
    }
}

std::string DescribeAnchorPlan(const AnchorPlan& plan) {
    std::ostringstream out;
    out << "mode=" << AnchorModeStr(plan.mode);
    auto list = [&out](const char* name, const std::vector<double>& v) {
        out << " " << name << "=[";
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out << ", ";
            out << v[i];
        }
        out << "]";
    };
    list("offsets", plan.offsets);
    std::vector<double> deltas;
    for (size_t i = 1; i < plan.offsets.size(); ++i) {
        deltas.push_back(plan.offsets[i] - plan.offsets[i - 1]);
    }
    list("deltas", deltas);
    list("residuals", plan.residuals);
    if (!plan.note.empty()) out << " note=" << plan.note;
    return out.str();
}

} // namespace SyncComms
