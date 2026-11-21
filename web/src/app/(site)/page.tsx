import Image from 'next/image';
import Link from 'next/link';
import stackedLogo from '../../../public/brand/logo-stacked.png';
import chordsShot from '../../../public/screens/chords.webp';
import composerShot from '../../../public/screens/composer.webp';
import mixerShot from '../../../public/screens/mixer.webp';
import producerRollShot from '../../../public/screens/producer-roll.webp';
import rackShot from '../../../public/screens/rack.webp';
import scoreShot from '../../../public/screens/score.webp';
import vcEqShot from '../../../public/screens/vc-eq.webp';

/**
 * The front page.
 *
 * **Every claim here is something the app does today**, drawn from the
 * architecture and roadmap (docs/00) and the "Done" steps of the UI build
 * order (docs/05) in the main repository. Nothing on the roadmap's "not yet"
 * list appears (collaboration has its own page), there are no testimonials,
 * and no prices: the price lives in
 * Stripe and is shown on the buying page. The screenshots are the app as it
 * is, never mocked up.
 *
 * The five headlines under "Built for the full orchestral session" are the
 * Memory Management spec's (§11), each checked against what shipped (docs/05,
 * steps 46-49). "Video" is left out of the unlimited-tracks headline: the
 * app has one picture track with cut versions, not stacked video tracks, and
 * only audio and MIDI were stress-tested. The footnote is the spec's own
 * wording, so the claim never implies unlimited real-time playback.
 */

const CLAIMS: ReadonlyArray<{ headline: string; body: string; proof: string }> = [
  {
    headline: 'Unlimited Audio & MIDI Tracks*',
    body:
      'No edition tiers and no track ceiling: nothing in the app, the project file or the licence counts your tracks. ' +
      'Build the whole template, every section and every stem, and let your computer be the only limit.',
    proof: 'Tested with 1,024 instrument and 2,048 audio tracks in one project: it saves in a fifth of a second and opens in a third.',
  },
  {
    headline: 'Compose Once. Your MIDI Is Already in Producer.',
    body:
      'Every staff you write is already a playing MIDI track in the Producer view, with the same notes, durations, dynamics ' +
      'and articulations, and nothing to enter twice. Switch views while it plays: the playhead, the selection and the transport carry on.',
    proof:
      'Change a written note in Producer and VC Composer asks: apply it to the score, or keep it as a Producer override. ' +
      'The page never silently disagrees with what you hear.',
  },
  {
    headline: 'Write with One Articulation Track—or Break It Out Into Dedicated Tracks.',
    body:
      'Keep a part on one track that switches articulations by keyswitch, controller or channel, or expand it into a track per ' +
      'sound (Violins I Staccato, Pizzicato, Tremolo) for editing, mixing and stems. The score keeps one staff either way.',
    proof: 'Every note plays exactly once, Consolidate puts the part back, and the orchestral templates come in both forms.',
  },
  {
    headline: 'Freeze Instruments to Free Up CPU and RAM.',
    body:
      'Freeze prints a track to audio through its instrument and inserts. Freeze & Unload then closes the instrument and gives ' +
      'its memory back, keeping its settings for when you unfreeze. Freeze one track, a part’s articulations or a whole family at once.',
    proof:
      'Each instrument runs in a process of its own, so the memory really is returned, and a print that no longer matches the music is marked stale.',
  },
  {
    headline: 'See CPU and RAM Usage While You Compose or Produce.',
    body:
      'The audio engine’s load, VC Composer’s memory and your computer’s sit at the bottom right of both views. ' +
      'Open Instrument Usage for every instrument’s memory and engine load, sorted the way you need, with Freeze, Suspend and Reload to hand.',
    proof: 'Measured, not guessed: where a plug-in’s memory cannot be told apart from another’s, it says so.',
  },
];

const FEATURES: ReadonlyArray<{ title: string; lede: string; points: string[]; warm?: boolean }> = [
  {
    title: 'Notation first',
    lede: 'You write on staves. The score is the source, and everything else is rendered from it.',
    points: [
      'Keyboard and mouse note entry, with note-value keys that follow Finale and Sibelius',
      'Engraving with the SMuFL Bravura font, print preview, PDF parts and full score',
      'Lyrics, repeats with endings, multi-bar rests, percussion maps, explode and implode',
      'Chord symbols, Chord Mode and instrument range guardrails while you write',
    ],
  },
  {
    title: 'One score, mirrored as MIDI',
    lede: 'Composer and Producer are two views of one project. Switching keeps the playhead and selection.',
    points: [
      'Change a written note in Producer and choose: apply it to the score, or keep it as a Producer override',
      'Overrides change how a note plays, never how it is printed, and Composer marks them',
      'Played-in MIDI is notated with quantize, voices and spelling, as one undo step',
      'Every instrument track carries its own audio lane: print the score to audio in place',
    ],
  },
  {
    title: 'A template for the cue in front of you',
    lede: 'Start from a palette built for the job rather than an empty session.',
    points: [
      'Cinematic, modern and production-era, regional and traditional, and historical-era templates',
      'Composer palettes, favorites, recent templates, and shareable template packages',
      'A New Project wizard that fills key, meter and tempo from the template',
      'Chord Explorer, Ostinato Creator and a Circle of Fifths with Roman numerals and borrowed chords',
    ],
    warm: true,
  },
  {
    title: 'Your instruments, your articulations',
    lede: 'Host the libraries you already own, and have every articulation land on the right sound.',
    points: [
      'VST3 instrument hosting on Windows and macOS, and Audio Units on macOS',
      'Plug-ins scanned and hosted out of process, with their state saved in the project',
      'Articulation maps with keyswitches, controllers, program changes, channel switching and velocity layers',
      'A library browser, and a built-in placeholder synth so templates play before a library is assigned',
    ],
  },
  {
    title: 'A hardware-style mixer',
    lede: 'A large-format console on screen, full screen when you want it, with the routing to match.',
    points: [
      'Channel strips with inserts, sends, EQ and dynamics, sidechain keying, VCAs and mix groups',
      'Automation in Read, Touch, Latch and Write, with lanes for every strip control and plug-in parameter',
      'Plug-in delay compensation, cue mixes, bounce in place, and a visual routing canvas',
      'Recording with count-in, pre-roll, punch and takes; loudness metering to EBU R 128 on the master',
    ],
  },
  {
    title: 'Scoring to picture',
    lede: 'A picture window locked to the transport, and the tools to land music on the cut.',
    points: [
      'Timecode at every film and TV rate, including 29.97 and 59.94 drop frame, and 35mm feet+frames',
      'Hits, streamers and punches, cut versions with offsets, and cut comparison',
      'A tempo finder that fits steady tempos to your hit points',
      'A cue manager with spotting notes and cue sheets, and picture exported with the score mix burned in',
    ],
    warm: true,
  },
  {
    title: 'Director review',
    lede: 'Put alternatives side by side and let the room choose, without losing any of them.',
    points: [
      'Track versions and whole-project snapshots, switched A/B/C while the transport plays',
      'Where the alternatives differ, per track and by bar',
      'Approval that locks a version, timecode comments with replies, and review mixes per slot',
      'Collect and Save Package: the project, its audio and a report of what the next machine needs',
    ],
  },
  {
    title: 'Deliverables',
    lede: 'Hand over what each person on the project needs, from the same session.',
    points: [
      'MusicXML import and export, and Standard MIDI File export',
      'Stems and the mix, per project or per cue, wet or dry, with a sum-to-mix check',
      'Render-by-cue WAV bounces and a master export',
      'PDF parts and full score straight from the engraved notation',
    ],
  },
];

type Screen = typeof composerShot;

/**
 * The screenshots are the app itself, captured at 2x by the desktop check's
 * marketing_shots section (desktop/check/DesktopCheck.cpp) from one demo
 * cue, "Night Train - Main Title": re-run it to refresh them.
 */
function Shot({ src, alt, priority = false, sizes }: { src: Screen; alt: string; priority?: boolean; sizes?: string }) {
  return (
    <figure className="shot">
      <Image src={src} alt={alt} priority={priority} sizes={sizes ?? '(max-width: 1120px) 100vw, 1120px'} placeholder="empty" />
    </figure>
  );
}

const VIEWS: ReadonlyArray<{ name: string; tag: string; lede: string; points: string[]; shot: Screen; alt: string }> = [
  {
    name: 'VC Composer',
    tag: 'Write',
    lede: 'Write on staves, one per instrument, with the keyboard or the mouse, and hear your libraries play it back. Switch to Score view to see the full score as it prints.',
    points: [
      'A track header per staff: record, solo, mute, volume, pan, instrument, MIDI in and out',
      'Solo plays one staff on its own; Score plays everything that is unmuted',
      'Page view in orchestral order, empty staves hidden, concert or transposed pitch',
      'The same engraving makes the PDF parts and the full score',
    ],
    shot: scoreShot,
    alt: 'Score view: two pages of the full score, woodwinds, brass, timpani, harp and strings in orchestral order',
  },
  {
    name: 'VC Producer',
    tag: 'Produce',
    lede: 'The same project as MIDI and audio. Every staff is already a playing track; open one and the notes you wrote are there in the piano roll, with their velocities.',
    points: [
      'Every instrument track carries its own audio lane, with its own record, solo and mute: MIDI, Audio or Both',
      'Recording an audio lane records that instrument, already wired in',
      'Edit how a note plays and choose: apply it to the score, or keep it as a Producer override',
      'A compact mixer under the tracks, with the inserts on every strip',
    ],
    shot: producerRollShot,
    alt: 'The Producer view: Violins I open in the piano roll with velocities, its printed audio lane beneath, the strings below, and a 21-strip compact mixer',
  },
];

const TOUR: ReadonlyArray<{ eyebrow: string; title: string; body: string; points: string[]; shot: Screen; alt: string; narrow?: boolean }> = [
  {
    eyebrow: 'Mixer',
    title: 'A large-format console in its own window',
    body:
      'The full mixer floats in a window of its own, so it can live on a second screen. Click a section title (Input, Filters, ' +
      'Equaliser, Dynamics, Inserts) to open it large and work on it, and click again to put it back.',
    points: [
      'Input trim and phase, high- and low-pass filters, four-band EQ, compressor and gate on every strip',
      'Group buses, FX buses and the master, with automation modes on every channel',
      'A compact deck docks under the Producer tracks when you only need faders',
    ],
    shot: mixerShot,
    alt: 'The channel console: fifteen orchestral channels with EQ, dynamics, pan and faders, the Woodwinds and Brass group buses, and the master',
  },
  {
    eyebrow: 'Rack gear',
    title: 'Inserts that look like the gear they are',
    body:
      'Open a strip’s inserts and they appear as rack-mount units, with real knobs and meters. Empty spaces read “Add unit”: ' +
      'click one to load another plug-in or piece of built-in gear, and expand the rack a bay at a time when you need more.',
    points: [
      'VC EQ, VC Compressor, VC Reverb, saturation and more, built in',
      'Your own VST3 and Audio Unit effects in the same rack',
      'Every knob can be automated',
    ],
    shot: rackShot,
    alt: 'A rack of VC gear: VC EQ, VC Compressor with a VU meter and VC Reverb, and an empty bay to add another unit',
    narrow: true,
  },
  {
    eyebrow: 'VC EQ',
    title: 'A parallel dynamic equaliser, built in',
    body:
      'Four bands that never fight each other: each is a low shelf, a bell or a high shelf, and any of them can turn dynamic, ' +
      'cutting only when its own frequencies get loud. A real-time analyser shows what goes in and what comes out.',
    points: [
      'Per-band threshold, ratio, attack and release, and a solo to hear one band alone',
      'Automatic gain compensation keeps the level where it was while you shape the tone',
      'External sidechain: let another track or bus trigger the dynamic bands',
      'High- and low-pass filters from 12 to 48 dB per octave',
    ],
    shot: vcEqShot,
    alt: 'VC EQ: the analyser with the input and output spectrum and the curve through four band nodes, and the band, filter and output controls',
    narrow: true,
  },
  {
    eyebrow: 'Chord Explorer',
    title: 'Build a progression, then send it as notation',
    body:
      'The key’s chords sit on top with their Roman numerals, and the borrowed chords from a mode you pick sit beneath. Drag ' +
      'them into a twelve-bar build, divide a bar, edit any chord’s notes, audition it, and send it to a staff.',
    points: [
      'Dozens of progressions to start from: pop, classical, jazz, blues, cinematic',
      'Two-hand voicings with voice leading',
      'Send one voice to a track, or spread all five over the strings, woodwinds, brass or choir',
    ],
    shot: chordsShot,
    alt: 'The Chord Explorer in D minor: diatonic and borrowed chords, the Romantic / cinematic progression, and an eight-bar build of i VI III VII i iv VI V',
  },
];

export default function HomePage() {
  return (
    <>
      <section className="home-hero">
        <div className="home-hero-top">
          <div>
            <span className="eyebrow">For film, TV and game composers</span>
            <h1>VC Composer, a notation-first scoring workstation.</h1>
            <p>
              Write on staves and hear your sample libraries play it. Edit the mirrored MIDI, mix on a hardware-style
              console, score to picture with timecode, hits and cues, run a director review, and export the parts, the
              MusicXML, the MIDI and the stems. One desktop application, on Windows and macOS. Not a plug-in.
            </p>
            <div className="actions">
              <Link href="/download" className="button">
                Subscribe and download
              </Link>
              <Link href="/download#preview" className="button secondary">
                Try the free preview
              </Link>
            </div>
          </div>
          <Image
            src={stackedLogo}
            alt="The VC Composer logo"
            className="home-hero-logo"
            priority
            sizes="(max-width: 720px) 300px, 380px"
          />
        </div>
        <Shot
          src={composerShot}
          priority
          alt="VC Composer with a fifteen-instrument orchestral cue in D minor: strings and woodwinds on their own staves, each with its track controls"
        />
      </section>

      <section aria-label="In numbers">
        <div className="figures">
          <div className="figure">
            <strong>205</strong>
            <span>templates: cinematic, modern, regional, era and composer palettes</span>
          </div>
          <div className="figure">
            <strong>314</strong>
            <span>instruments in the registry, each with its range and articulations</span>
          </div>
          <div className="figure">
            <strong>2 views</strong>
            <span>Composer and Producer, over one project and one track list</span>
          </div>
          <div className="figure">
            <strong>2 platforms</strong>
            <span>Windows and macOS, one licence covering both</span>
          </div>
        </div>
      </section>

      <section id="views" aria-labelledby="views-title">
        <span className="eyebrow">Two views, one project</span>
        <h2 id="views-title">VC Composer and VC Producer</h2>
        <p className="lede">
          Write in Composer, produce in Producer. They are two views of the same tracks, so a note written on a staff is already
          playing in Producer, and switching keeps the playhead and the selection.
        </p>
        {VIEWS.map((view) => (
          <article key={view.name} className="showcase">
            <div className="showcase-text">
              <span className="showcase-tag">{view.tag}</span>
              <h3>{view.name}</h3>
              <p>{view.lede}</p>
              <ul>
                {view.points.map((point) => (
                  <li key={point}>{point}</li>
                ))}
              </ul>
            </div>
            <Shot src={view.shot} alt={view.alt} />
          </article>
        ))}
      </section>

      <section id="tour" aria-labelledby="tour-title">
        <span className="eyebrow">A closer look</span>
        <h2 id="tour-title">From the first note to the final mix</h2>
        <div className="tour">
          {TOUR.map((stop, at) => (
            <article key={stop.title} className={`tour-stop${at % 2 ? ' flip' : ''}${stop.narrow ? ' narrow' : ''}`}>
              <div className="tour-text">
                <span className="eyebrow">{stop.eyebrow}</span>
                <h3>{stop.title}</h3>
                <p>{stop.body}</p>
                <ul>
                  {stop.points.map((point) => (
                    <li key={point}>{point}</li>
                  ))}
                </ul>
              </div>
              <Shot src={stop.shot} alt={stop.alt} sizes="(max-width: 900px) 100vw, 640px" />
            </article>
          ))}
        </div>
      </section>

      <section id="sessions" aria-labelledby="sessions-title">
        <span className="eyebrow">Big sessions, everyday computers</span>
        <h2 id="sessions-title">Built for the full orchestral session</h2>
        <p className="lede">
          Large templates are where most setups make you choose between the score and the session. VC Composer is built so you
          do not have to.
        </p>
        <ol className="claims">
          {CLAIMS.map((claim) => (
            <li key={claim.headline} className="claim">
              <h3>{claim.headline}</h3>
              <div>
                <p>{claim.body}</p>
                <p className="proof">{claim.proof}</p>
              </div>
            </li>
          ))}
        </ol>
        <p className="footnote" id="track-footnote">
          * No software-imposed track-count limits. Performance depends on your computer, plugins, storage and project
          configuration.
        </p>
      </section>

      <section id="features">
        <h2>What it does</h2>
        <p className="lede">Everything below is in the app today.</p>
        <div className="feature-grid">
          {FEATURES.map((feature) => (
            <article key={feature.title} className={feature.warm ? 'feature warm' : 'feature'}>
              <h3>{feature.title}</h3>
              <p className="lede">{feature.lede}</p>
              <ul>
                {feature.points.map((point) => (
                  <li key={point}>{point}</li>
                ))}
              </ul>
            </article>
          ))}
        </div>
      </section>

      <section>
        <h2>Built to keep your work</h2>
        <div className="grid">
          <article className="card">
            <h3>Crash-safe saving</h3>
            <p className="lede">Atomic saves and autosave with recovery, and a newer project file is refused rather than half-read.</p>
          </article>
          <article className="card">
            <h3>Never silent</h3>
            <p className="lede">A missing library becomes a labelled placeholder with a message, and a collision asks before it overwrites.</p>
          </article>
          <article className="card">
            <h3>Relink missing media</h3>
            <p className="lede">Moved drives and other machines: the app searches for audio and picture and relinks them in one pass.</p>
          </article>
        </div>
      </section>

      <section className="cta-band">
        <div>
          <h2>Collaboration</h2>
          <p>
            Share a project, audio and picture included, with your orchestrator, music editor and director: sync with
            merge, roles and part locks. Its own subscription, per editor seat; reviewers join free.
          </p>
        </div>
        <Link href="/collaboration" className="button secondary">
          Read more
        </Link>
      </section>

      <section className="cta-band">
        <div>
          <h2>Start writing</h2>
          <p>Subscribe monthly or yearly, or try the free preview first.</p>
        </div>
        <div className="actions">
          <Link href="/download" className="button">
            Subscribe
          </Link>
          <Link href="/download#preview" className="button warm">
            Free preview
          </Link>
        </div>
      </section>
    </>
  );
}
