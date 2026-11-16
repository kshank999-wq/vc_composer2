import AVFoundation
import Combine

/// The iPad's own lightweight audition (spec §6.2): Apple's built-in sampler,
/// not the desktop's sample libraries, which stay on the desktop.
final class Player: ObservableObject {
    @Published private(set) var position: Double? = nil  // seconds, while playing

    private let engine = AVAudioEngine()
    private let sampler = AVAudioUnitSampler()
    private var timer: Timer?
    private var notes: [VCCPlayedNote] = []
    private var next = 0
    private var sounding: [(end: Double, pitch: UInt8)] = []
    private var started = Date()

    init() {
        engine.attach(sampler)
        engine.connect(sampler, to: engine.mainMixerNode, format: nil)
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
    }

    var playing: Bool { timer != nil }

    func play(_ document: ScoreDocument) {
        stop()
        notes = document.core.performance()
        guard !notes.isEmpty else { return }
        try? AVAudioSession.sharedInstance().setActive(true)
        if !engine.isRunning { try? engine.start() }
        next = 0
        started = Date()
        position = 0
        timer = Timer.scheduledTimer(withTimeInterval: 0.005, repeats: true) { [weak self] _ in self?.tick() }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
        for s in sounding { sampler.stopNote(s.pitch, onChannel: 0) }
        sounding = []
        position = nil
    }

    private func tick() {
        let now = Date().timeIntervalSince(started)
        position = now
        sounding.removeAll { s in
            if s.end <= now { sampler.stopNote(s.pitch, onChannel: 0); return true }
            return false
        }
        while next < notes.count, notes[next].start <= now {
            let n = notes[next]
            let pitch = UInt8(clamping: n.pitch)
            sampler.startNote(pitch, withVelocity: UInt8(clamping: n.velocity), onChannel: 0)
            sounding.append((n.start + n.duration, pitch))
            next += 1
        }
        if next >= notes.count && sounding.isEmpty { stop() }
    }
}
