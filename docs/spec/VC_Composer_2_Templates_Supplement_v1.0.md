Supplemental Development Specification • Version 1.0 • 11 November 2026

Audience: coding agent / product engineering team. Scope: add-on specification to the VC Composer 2 desktop scoring/sequencer specification, not a replacement.

# 1. Executive directive

Implement a searchable, editable library of one-click score templates that prebuild a cue/project with correctly named and organized instrument tracks, default articulations, routing and musical starting points. Add two independent composition assistants: a key-aware Chord Explorer/Progression Builder and an auditionable Ostinato Creator. All functionality must run inside the installed macOS and Windows desktop application and integrate with the existing sequencer, instrument/plugin hosting, detachable mixer, inserts, buses, and reference/temp-track workflow.

Crucial distinction: a template defines instrument roles and instrument/plugin mappings; it does NOT imply that the application ships licensed sample libraries. If a requested patch is unavailable, keep its labeled track and provide a substitute/placeholder rather than silently changing the orchestration.

# 2. Scope and delivery boundaries

- In scope: cinematic/emotional score palettes; culturally and regionally grounded ensembles; historical-period and named-composer reference palettes; blues/jazz/rock/electronic/industrial and era-specific modern styles; built-in progression catalog; chord voicing explorer; MIDI audition; 4/8-bar chord placement; ostinato generation and audition; insertion into selected tracks; saving/exporting custom templates.

- Out of scope: automatic imitation of any living artist’s identifiable recordings or compositions; bundled third-party samples without distribution rights; automatic fully orchestrated score generation; cloud dependence for core template/chord/ostinato use.

- Preserve user control: template selection never overwrites an existing session without explicit replace/merge decision; all resulting notes, articulations, instruments and routes are editable.

# 3. User-facing workflow

1.  New Project → Choose Blank Project or Browse Templates. Filter by category, mood, culture/region, era/composer, instrumentation, ensemble size and available plugins.

2.  Open template preview: tracks/instruments, articulations, buses, expected patch requirements, optional tempo/meter/key suggestions, cultural/historical notes and a short unlicensed MIDI-generated audition.

3.  Choose Create, Add to Existing Project or Save As New Custom Template; choose target plugin mapping and unavailable-patch handling.

4.  Generated track tree appears in sequencer with sections, descriptive names, articulation lanes/key switches (when supported), color grouping and mixer channels/buses. Temp/reference audio remains a separate optional track, never part of template playback by default.

5.  Open Chord Explorer: select project key/mode, pick progression, 4 or 8 bars, audition chord quality/voicing/inversion, send MIDI to a chosen track/new harmony track.

6.  Open Ostinato Creator: choose track/instrument, key/chord-follow setting, rhythm, density, duration (e.g. 16 bars), audition variants A/B/C, then Send to Track.

# 4. Template taxonomy: cinematic narrative and emotional

Each line is a seed preset, not a stereotype or a fixed orchestration. The library should allow multiple subvariants (acoustic, hybrid, minimal, large orchestra) of each mood.

| Starter template              | Default instrumentation                                                                           | Articulation / treatment                                                 |
|-------------------------------|---------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------|
| Horror — psychological        | Low strings sul ponticello/tremolo; bass clarinet; contrabassoon; prepared piano; granular drones | Clusters, aleatoric swells, glissandi, col legno, harmonics; sparse hits |
| Horror — supernatural/gothic  | Organ, choir, low brass, strings, celesta, tubular bells                                          | Pedal points, divisi clusters, tremolo, ominous unisons                  |
| Suspense / tension            | Muted low strings, ostinato violas/cellos, pulses, bass clarinet, ticking percussion              | Sul tasto, measured tremolo, repeating cells, crescendos                 |
| Thriller / pursuit            | Short strings, low brass, synth bass, taiko/low drums, metallic impacts                           | Spiccato, marcato, syncopated ostinati, brass stabs                      |
| Mystery / investigation       | Piano, clarinet, vibraphone, harp, muted trumpet, chamber strings                                 | Sparse motifs, pizzicato, soft mallets, suspended harmony                |
| Comedy — light/quirky         | Pizzicato strings, oboe, bassoon, clarinet, xylophone, piano                                      | Pizzicato, staccato, grace notes, short runs                             |
| Comedy — slapstick            | Bassoon, trombone, clarinet, tuba, woodblock, xylophone                                           | Glissandi, falls, accents, rapid chromatic figures                       |
| Whimsical / playful fantasy   | Flute/piccolo, celesta, harp, pizz strings, glockenspiel                                          | Trills, flutters, arpeggios, light staccato                              |
| Romance / love                | Solo violin/viola/cello, legato strings, piano, harp, warm woodwinds                              | Legato, portamento sparingly, lyrical suspensions                        |
| Heartbreak / grief            | Solo cello, felt piano, soft strings, English horn                                                | Long sustains, expressive dynamics, sparse voicing                       |
| Sad / reflective              | Piano, solo strings, clarinet, ambient pads                                                       | Soft legato, restrained motifs, wide rests                               |
| Hope / uplift                 | Piano, string ensemble, French horns, warm synth pad                                              | Gradual layering, legato build, broad voicings                           |
| Heroic / triumph              | Horns, trumpets, trombones, full strings, timpani, cymbals                                        | Marcato brass, soaring legato strings, rhythmic ostinati                 |
| Epic / large-scale            | Full orchestra, choir, percussion ensemble, synth layers                                          | Divisi, ostinati, brass fanfares, percussion builds                      |
| Action / combat               | Short strings, brass, low winds, drums, pulse synths                                              | Spiccato, staccatissimo, accents, fast ostinati                          |
| Adventure / discovery         | Full strings, horns, flutes, woodwinds, harp, percussion                                          | Broad themes, arpeggiated figures, sweeping runs                         |
| Fantasy / magic               | Celesta, harp, choirs, flutes, strings, tuned percussion                                          | Harmonics, tremolo shimmer, harp glissandi                               |
| Sci-fi / exploration          | Analog-style synths, hybrid strings, pads, processed percussion                                   | Slow filter motion, pulses, evolving drones                              |
| Dystopian / industrial sci-fi | Distorted synth bass, metal percussion, processed strings, noise layers                           | Gated pulses, polyrhythms, dissonant pads                                |
| War / military drama          | Snare, low brass, strings, timpani, solo trumpet                                                  | Measured rolls, restrained ostinati, chorale writing                     |
| Historical / period drama     | Era-selectable authentic ensemble, optional modern cinematic layer                                | Period-dependent ornaments and articulation sets                         |
| Documentary / human interest  | Piano, chamber strings, acoustic guitar, pads, restrained percussion                              | Minimal motifs, transparent textures                                     |
| Nature / pastoral             | Flute, oboe, harp, acoustic guitar, strings                                                       | Legato, open intervals, flowing arpeggios                                |
| Noir / detective              | Muted trumpet, saxophone, upright bass, brushed drums, piano                                      | Falls, soft vibrato, jazz voicings                                       |
| Western / frontier            | Acoustic guitar, harmonica, fiddle, low strings, whistle, drums                                   | Strums, tremolo guitar, open-string drones                               |
| Sports / victory              | Drumline, horns, strings, bass, synth accents                                                     | Rhythmic unison, fanfare, punchy stabs                                   |
| Ambient / meditation          | Soft pads, piano, mallet textures, sustained strings                                              | Slow attack, sparse phrases, drones                                      |

# 5. Culture, region, and traditional-ensemble library

Use specific regional, historical, or ensemble names rather than a monolithic “ethnic” preset. Labels and descriptions should distinguish traditional ensemble, contemporary fusion, and cinematic interpretation. Instruments vary by locality, era and practice; make tuning, temperament, scale and performance techniques configurable. Provide attribution/context notes, not blanket authenticity claims.

| Region / ensemble                                | Track palette                                                                | Technique / context                                                            |
|--------------------------------------------------|------------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| Ireland — traditional                            | Fiddle, tin whistle, uilleann pipes, flute, bouzouki, bodhrán                | Reels/jigs; rolls, cuts, bowed ornaments                                       |
| Scotland — traditional                           | Great Highland bagpipes or smallpipes, fiddle, harp/clàrsach, frame drum     | Drones, grace-note ornaments; instrument-specific tuning                       |
| Wales — folk                                     | Triple harp, fiddle, crwth (historical), voice                               | Harp patterns; historical mode choice                                          |
| England — folk / medieval                        | Fiddle, concertina (later), lute, recorder, frame drum                       | Style/era-dependent; avoid anachronistic defaults                              |
| Germany — folk / Alpine                          | Accordion, zither, brass ensemble, clarinet, double bass                     | Dance pulses, oom-pah accompaniments                                           |
| Early Germanic / medieval Central Europe         | Lyre, reconstructed frame drums, bone/wood flutes, voice                     | Explicitly tagged speculative reconstruction; no claim of exact original sound |
| Nordic — Scandinavian folk                       | Hardanger fiddle, nyckelharpa, kantele (Finnish), tagelharpa (reconstructed) | Drones, ornaments, asymmetric bowing                                           |
| Spain — Iberian folk                             | Flamenco guitar, palmas, cajón (modern flamenco), voice                      | Rasgueado, golpe, compás; distinguish region                                   |
| France — folk / musette                          | Accordion, fiddle, hurdy-gurdy, flute                                        | Bourdons, dance rhythms                                                        |
| Italy — folk                                     | Mandolin, accordion, tamburello, guitar                                      | Tremolo picking, tarantella patterns                                           |
| Balkans — region-specific                        | Clarinet, kaval, gaida, accordion, tapan                                     | Odd-meter options; choose local tradition                                      |
| Greece — rebetiko / folk                         | Bouzouki, baglamas, guitar, violin                                           | Modal phrases, tremolo, drones                                                 |
| Türkiye — makam-oriented                         | Oud, bağlama, ney, kanun, darbuka                                            | Makam/microtonal tuning; ornaments                                             |
| Arab traditions — selected ensemble              | Oud, qanun, ney, riq, darbuka, violin                                        | Maqam-aware pitch bends and ornaments                                          |
| Iran — Persian classical                         | Tar, setar, santur, kamancheh, ney, tombak                                   | Dastgah/radif-aware melodic organization                                       |
| India — Hindustani                               | Sitar/sarod, bansuri, tabla, tanpura, harmonium (where applicable)           | Raga/drone and tala; slides/gamak                                              |
| India — Carnatic                                 | Veena/violin, mridangam, ghatam, kanjira, tambura                            | Raga, tala, gamaka articulation                                                |
| China — traditional chamber                      | Erhu, pipa, guzheng, dizi, sheng, yangqin                                    | Slides, bends, plucks, ornamented phrases                                      |
| Japan — traditional                              | Koto, shakuhachi, shamisen, taiko (ensemble-specific)                        | Breathy attacks, slides, plucks, ma/space                                      |
| Korea — traditional                              | Gayageum, geomungo, daegeum, haegeum, janggu                                 | Nonghyeon vibrato and rhythmic cycles                                          |
| Indonesia — Javanese gamelan                     | Metallophones, gongs, kendhang, suling                                       | Colotomic cycles; native tuning sets                                           |
| Indonesia — Balinese gamelan                     | Gangsa, reyong, gongs, kendang                                               | Interlocking kotekan; local tuning                                             |
| Thailand — piphat                                | Ranat, khong wong, pi, taphon, ching                                         | Cyclic ensemble patterns                                                       |
| Vietnam — traditional                            | Đàn tranh, đàn bầu, đàn nguyệt, sáo trúc                                     | Bends/ornamentation; regional variants                                         |
| West Africa — Mandé-inspired                     | Kora, balafon, djembe, dunun                                                 | Interlocking ostinati; identify cultural lineage                               |
| West Africa — Yoruba-related                     | Talking drum/dùndún, bàtá, bells, voice                                      | Speech-tone/drumming relationship; respect context                             |
| Ghana — Ewe percussion                           | Gankogui, axatse, drum ensemble                                              | Cross-rhythms and timeline patterns                                            |
| Central Africa — selected traditions             | Likembe/lamellophone, drums, voice                                           | Interlocking patterns; specific community metadata                             |
| Southern Africa — selected traditions            | Mbira (Zimbabwe), hosho, marimba, vocals                                     | Interlocking cycles; mbira tuning                                              |
| East Africa — selected traditions                | Endongo, nyatiti, regional drums and vocals                                  | Community-specific scales and ensemble setup                                   |
| North Africa — Maghrebi                          | Oud, bendir, guembri (Gnawa), qraqeb                                         | Grooves and call/response; subgenre choice                                     |
| Afro-Cuban                                       | Congas, bongos, timbales, claves, piano, bass, horns                         | Clave-aligned rhythms and montuno                                              |
| Brazil — samba/choro                             | Pandeiro, cavaquinho, seven-string guitar, surdo                             | Samba subdivision; choro counterpoint                                          |
| Argentina — tango                                | Bandoneón, violin, piano, double bass, guitar                                | Marcato, arrastre, syncopation                                                 |
| Mexico — mariachi                                | Violins, trumpets, vihuela, guitarrón, harp (regional)                       | Rasgueado and regional song rhythms                                            |
| Andes — selected ensemble                        | Quena, siku/zampoña, charango, bombo, guitar                                 | Panpipe hocketing, strums                                                      |
| Caribbean — steelband                            | Steel pans, engine-room percussion, bass                                     | Calypso/soca patterns, rolls                                                   |
| North American bluegrass                         | Fiddle, banjo, mandolin, acoustic guitar, upright bass                       | Rolls, chop rhythm, flatpicking                                                |
| North American Indigenous — curated partnerships | No universal default; community-approved instruments and metadata only       | Opt-in curated culturally specific sets; protect restricted material           |

# 6. Historical era and classical-composer reference templates

Templates are historically informed starting palettes, not simulations of a composer’s full musical language. Show composer birth/death dates and example genres/works in the library metadata when curated. Repertoire changed over each career: include a “work/period variant” option. Avoid assigning later instruments to earlier pieces unless explicitly tagged cinematic adaptation.

# 6.1 Era presets

| Period                               | Representative scoring palette                                                | Implementation note                                    |
|--------------------------------------|-------------------------------------------------------------------------------|--------------------------------------------------------|
| Medieval (approx. 1000–1400)         | Voices, organum, organ, vielle, harp, recorder, lute (context-specific)       | Regional/century-dependent; reconstruction limitations |
| Renaissance (approx. 1400–1600)      | Vocal consort, viol consort, recorders, cornett/sackbut, lute, organ          | Sacred/secular and court/church variants               |
| Early Baroque (approx. 1600–1700)    | Strings, basso continuo (harpsichord/organ/theorbo/cello), recorders, cornett | Figured bass; historical temperament option            |
| Late Baroque (approx. 1700–1750)     | Strings, continuo, oboe/bassoon, trumpet/timpani where appropriate            | Ornaments; concerto and cantata variants               |
| Classical (approx. 1750–1820)        | Strings, pairs of winds, horns, trumpets/timpani as needed                    | Ensemble scales by date and work                       |
| Early Romantic (approx. 1800–1850)   | Expanded strings, more independent winds/brass, timpani                       | Transition away from continuo                          |
| Late Romantic (approx. 1850–1910)    | Expanded orchestral winds/brass, chromatic harmony, harp/percussion           | Large-scale late-Romantic variants                     |
| Impressionist / turn-of-century      | Colorful winds, strings, harp, celesta, tuned percussion                      | Transparent textures, extended color                   |
| Early Modernist (approx. 1900–1945)  | Flexible orchestra, chamber, percussion and piano ensembles                   | Work-specific variable instrumentation                 |
| Postwar / contemporary concert music | Ensemble-dependent acoustics, electronics, extended techniques                | Do not assume a single standard orchestra              |

# 6.2 Named-composer preset catalog (initial breadth)

| Composer                  | Reference style                             | Typical starter instrumentation                               |
|---------------------------|---------------------------------------------|---------------------------------------------------------------|
| Hildegard of Bingen       | Medieval vocal monody; voice/drone optional | Voice-led; modern accompaniment clearly labeled               |
| Guillaume de Machaut      | Medieval vocal polyphony                    | Voice consort; instrumental doubling optional                 |
| Josquin des Prez          | Renaissance sacred vocal polyphony          | SATB vocal consort                                            |
| Giovanni P. da Palestrina | Renaissance choral counterpoint             | Vocal choir, optional organ doubling                          |
| Claudio Monteverdi        | Early Baroque opera/madrigal                | Voices, continuo, strings, period winds                       |
| Heinrich Schütz           | Early Baroque sacred concerted music        | Voices, continuo, sackbuts/cornetts as appropriate            |
| Arcangelo Corelli         | Baroque chamber/concerto grosso             | Violins, viola, continuo                                      |
| Antonio Vivaldi           | Late Baroque concertos                      | Solo violin/other soloists, strings, continuo                 |
| J. S. Bach                | Baroque cantata/concerto/organ              | Strings, continuo, oboes, flutes, organ, choir by work        |
| G. F. Handel              | Baroque oratorio/opera                      | Choir, strings, winds, trumpet/timpani, continuo              |
| Domenico Scarlatti        | Baroque keyboard sonata                     | Harpsichord                                                   |
| Jean-Philippe Rameau      | French Baroque opera                        | French orchestra, continuo, dance winds                       |
| C. P. E. Bach             | Empfindsamer Stil                           | Keyboard, chamber orchestra                                   |
| Joseph Haydn              | Classical symphony/quartet                  | Strings, paired winds, horns; brass/timpani by work           |
| W. A. Mozart              | Classical opera/concerto/symphony           | Strings, woodwinds, horns, trumpets/timpani as appropriate    |
| Ludwig van Beethoven      | Classical-to-Romantic orchestra             | Expanded symphony orchestra; period-specific                  |
| Franz Schubert            | Lieder/chamber/symphony                     | Voice+piano or Classical/Romantic orchestra                   |
| Felix Mendelssohn         | Early Romantic orchestra                    | Strings, woodwinds, brass, timpani                            |
| Frédéric Chopin           | Romantic piano works                        | Solo piano; piano+orchestra subset                            |
| Robert Schumann           | Romantic piano/chamber/orchestra            | Piano, strings and orchestra subsets                          |
| Hector Berlioz            | Romantic programmatic orchestra             | Large orchestral brass, winds, strings, percussion            |
| Franz Liszt               | Romantic piano/symphonic poem               | Solo piano or enlarged Romantic orchestra                     |
| Giuseppe Verdi            | Romantic opera                              | Voices, orchestra, operatic pit organization                  |
| Richard Wagner            | Music drama                                 | Expanded brass, Wagner tubas in selected works, large strings |
| Johannes Brahms           | Late Romantic orchestra                     | Rich strings, paired winds, horns, brass                      |
| Anton Bruckner            | Late Romantic symphony                      | Large orchestra, brass chorales                               |
| Pyotr I. Tchaikovsky      | Romantic ballet/symphony                    | Expanded strings, winds, brass, harp, percussion              |
| Antonín Dvořák            | Romantic orchestral/chamber                 | Strings, winds, horns, brass, percussion                      |
| Gustav Mahler             | Late Romantic symphony                      | Large orchestra, expanded brass/percussion, optional voice    |
| Richard Strauss           | Late Romantic tone poem/opera               | Large wind/brass forces and elaborate strings                 |
| Claude Debussy            | French colorist orchestration               | Woodwinds, strings, harp, percussion                          |
| Maurice Ravel             | Early modern colorist orchestra             | Woodwinds, harp, strings, tuned percussion                    |
| Jean Sibelius             | Nordic late Romantic/modern                 | Strings, winds, brass; long pedal textures                    |
| Alexander Scriabin        | Late Romantic/modern piano-orchestra        | Piano/large orchestra; period variant                         |
| Sergei Rachmaninoff       | Late Romantic piano/orchestra               | Virtuosic piano, lush strings and orchestra                   |
| Igor Stravinsky           | Early modern rhythm/color                   | Ballet orchestra or neoclassical chamber ensemble             |
| Béla Bartók               | 20th-century modernism                      | String quartet/orchestra/percussion+piano                     |
| Sergei Prokofiev          | 20th-century symphonic/ballet               | Agile winds, piano, brass, strings                            |
| Dmitri Shostakovich       | 20th-century symphony/quartet               | Large orchestra or string quartet                             |
| Arnold Schoenberg         | Expressionist/serial chamber-orchestra      | Chamber or orchestral instrumentation by work                 |
| Alban Berg                | Expressionist opera/chamber                 | Orchestra/chamber hybrids by work                             |
| Anton Webern              | Sparse chamber orchestration                | Small pointillist ensembles                                   |
| Olivier Messiaen          | 20th-century organ/orchestra                | Organ, piano, winds, strings, percussion                      |
| Benjamin Britten          | 20th-century opera/chamber orchestra        | Chamber orchestra, voice, period work variants                |
| Aaron Copland             | American modernist/pastoral orchestra       | Orchestra, brass/woodwind clarity                             |
| Samuel Barber             | American lyrical orchestral                 | Strings/chamber/orchestra                                     |
| Florence Price            | American symphonic tradition                | Orchestra, piano, organ by work                               |
| William Grant Still       | American symphonic tradition                | Orchestra with jazz/blues influences                          |
| Duke Ellington            | Jazz orchestra / concert works              | Saxes, brass, rhythm section; piano                           |
| John Cage                 | Experimental / prepared piano               | Prepared piano, percussion/variable ensembles                 |
| Steve Reich               | Minimalist ensemble                         | Piano/mallets/voice/repeating cells                           |
| Philip Glass              | Minimalist / postminimalist                 | Keyboards, winds, strings, repeated patterns                  |
| Arvo Pärt                 | Tintinnabuli / sacred minimalism            | Voices, strings, organ/piano by work                          |
| John Adams                | Contemporary orchestral minimalism          | Orchestra, synths/percussion by piece                         |

Future expansion targets: additional medieval/renaissance, non-European classical lineages, 20th-century orchestral composers, opera and chamber traditions. Provide a versioned content manifest so new composer packs can ship without binary changes.

# 7. Modern, popular, and production-era templates

| Template                       | Track palette                                                       | Style handles                            |
|--------------------------------|---------------------------------------------------------------------|------------------------------------------|
| Delta / country blues          | Acoustic slide guitar, voice, harmonica, bass optional              | Shuffle/12-bar variants                  |
| Chicago electric blues         | Electric guitar, harmonica, bass, drums, piano/organ                | Turnarounds, bent notes                  |
| Jump blues / early R&B         | Horn section, piano, guitar, bass, drums                            | Swing/shuffle                            |
| Ragtime                        | Piano                                                               | Syncopated right hand, march bass        |
| Traditional / New Orleans jazz | Trumpet/cornet, clarinet, trombone, tuba/bass, banjo, piano, drums  | Collective improvisation roles           |
| Swing / big band (1930s–40s)   | Sax section, trumpet/trombone sections, piano, bass, drums, guitar  | Section voicings, swing feel             |
| Bebop (1940s–50s)              | Alto/tenor sax, trumpet, piano, upright bass, drums                 | Fast ii–V, altered dominants             |
| Cool / West Coast jazz         | Sax, trumpet, piano, bass, drums; optional chamber winds            | Spacious voicings                        |
| Hard bop / soul jazz           | Horn-led quintet, piano/Hammond, bass, drums                        | Blues/gospel harmony                     |
| Modal jazz                     | Sax/trumpet, piano, bass, drums                                     | Modal vamps                              |
| Latin jazz / Afro-Cuban jazz   | Horns, piano, bass, congas, timbales, clave                         | Clave-aware harmonic/rhythmic grid       |
| Jazz fusion (1970s)            | Electric piano, synth, electric guitar/bass, drums                  | Complex meter and extended chords        |
| Contemporary jazz / neo-soul   | Keys/Rhodes, bass, drums, guitar, horns                             | Rich extensions, laid-back pocket        |
| Rock & roll (1950s)            | Electric guitar, upright/electric bass, drums, piano/sax            | Backbeat                                 |
| Classic rock (1960s–70s)       | Electric guitars, bass, acoustic drums, organ                       | Riffs, power chords                      |
| Progressive rock (1970s)       | Guitars, bass, drums, analog synth, organ                           | Odd meters, long forms                   |
| Punk / garage rock             | Distorted guitar, bass, drums, shout vocals optional                | Fast, direct rhythms                     |
| Hard rock / metal              | High-gain guitars, bass, drums, synth optional                      | Palm-mute and double-kick                |
| Alternative / grunge (1990s)   | Guitars, bass, drums, ambient effects                               | Dynamic contrast                         |
| Indie / post-rock              | Clean/delayed guitars, bass, drums, pads                            | Crescendo, evolving texture              |
| Funk / soul (1960s–70s)        | Drums, bass, guitar, clavinet, Hammond, horns                       | Sixteenth-note pocket                    |
| Disco (1970s)                  | Four-on-floor drums, strings, bass, rhythm guitar, keys             | Octave bass, syncopated rhythm           |
| Synth-pop / new wave (1980s)   | Analog polysynth, drum machine, bass synth, guitar                  | Sequenced arpeggios                      |
| Hip-hop — boom bap             | Sample-style drums, bass, keys, turntable FX                        | Swing, 8/16-bar loops                    |
| Hip-hop — trap                 | 808 bass, hats, snare/clap, synth/keys                              | Hat rolls, half-time                     |
| Ambient electronic             | Pads, granular texture, soft pulses                                 | Slow evolution                           |
| Techno / house                 | Drum machine, bass synth, stab chords, sequencer                    | Four-on-floor, sync                      |
| Drum & bass                    | Breakbeats, sub-bass, pads, leads                                   | Fast syncopated breaks                   |
| Industrial / dark electronic   | Distorted synths, metallic percussion, noise, processed guitar      | Modulation, saturation, transient design |
| Minimal dark cinematic hybrid  | Felt piano, analog noise, sub-bass, processed strings, sparse drums | Evolving drone, irregular pulse          |
| Contemporary film-score hybrid | Orchestra, synth textures, cinematic impacts, piano                 | Layered acoustic/electronic cues         |
| Lo-fi / chillhop               | Dusty kit, Rhodes, bass, sampled textures                           | Swing and tape character                 |
| Modern pop / ballad            | Piano/guitar, synth pads, bass, drums                               | Hook-driven harmony                      |

Artist-reference guardrail: use descriptive traits (e.g., “industrial ambient tension,” “minimal electronic drama”) rather than one-click named living-artist imitation presets. Historical composer reference presets above should be framed as educational orchestration palettes, not claims to reproduce original compositions.

# 8. Template data contract and project generation

Store shipped presets as versioned declarative JSON/YAML content, validated against schema. No baked-in vendor-specific plugins. Suggested content fields:

template_id, schema_version, content_version, display_name, category, tags\[\], locale,  
historical_period?, composer_id?, cultural_context?, authenticity_note?, variant,  
recommended_key_modes\[\], tempo_range_bpm, meter_options\[\], default_length_bars?,  
tracks\[\]: {id, name, family, role, instrument_standard_id, transposition,  
plugin_requirements\[\], fallback_instruments\[\], articulation_map\[\],  
color, group_id, midi_channel_policy, default_gain_pan, output_bus_id},  
buses\[\]: {id, name, sends\[\], insert_suggestions\[\]},  
progression_ids\[\], ostinato_preset_ids\[\], preview_midi?, license_metadata.

- Track creation: ordered folders by instrumental family (Strings, Woodwinds, Brass, Percussion, Keys, Synth, Traditional, Rhythm, Vocals); editable child tracks and optional expression/keyswitch lanes.

- Instrument resolution: plugin/preset catalog maps a standardized instrument ID and articulation to installed instruments; choose exact mapping, approved fallback, or silent placeholder. Never make missing patches fatal.

- Articulation support: native keyswitch, MIDI CC, channel selection, expression map and per-note technique metadata; surface incompatible patches clearly. Avoid playing keyswitch notes as pitched music.

- Routing: create default section buses and master routes; configure inserts/sends as suggestions unless the plugin is installed; mixer pop-out reflects generated channels immediately.

- Project merge: avoid duplicate names through stable IDs and user-friendly name suffixes; preserve existing arrangement, routing and temp/reference track; undo the entire insert as a single operation.

- Persistence: save editable built-in-derived custom templates separately; mark immutable shipped templates; support export/import with validation, migration and license metadata.

- Preview: generate copyright-safe MIDI snippets or use only licensed embedded demonstration assets. Preview must be cancelable and not alter project transport.

# 9. Chord Explorer and Progression Builder

## 9.1 Pop-up design and controls

- Open from toolbar, selected MIDI region, track context menu and template preview. Show current project key and mode; change local preview key without changing project key until Apply.

- Chord selection: triads (major, minor, diminished, augmented, sus2/sus4), sixths, sevenths (maj7, min7, dom7, half-diminished, dim7, min-major7), ninths, elevenths, thirteenths, add9, altered dominants; selectable omit/add tones.

- Voicing and inversion: root position, first/second/third inversion as applicable, closed/open/spread, drop-2/drop-3 where practical, octave, register, voice-leading smoothness, bass note/slash chords; intelligent transposition to selected key.

- Progression browser: genre/mood, major/minor/modal, 2/4/8/12/16-bar patterns, cadence type, Roman numeral view and chord-name view. User requested default 4 or 8 bars; support extension as future-compatible option.

- Audition: play/stop, loop, tempo, click/metronome, piano/strings or chosen installed instrument, A/B compare and chord-by-chord audition; show piano keyboard and voicing pitches.

- Send to Track: create/edit MIDI region for precisely 4/8 bars at selected start bar with selectable chord rhythm, strum/arpeggiation or block-chord style, velocity and legato; choose existing MIDI instrument track or new harmony track; undoable.

## 9.2 Initial progression catalog

| Family               | Roman-numeral example            | Use / variants                                |
|----------------------|----------------------------------|-----------------------------------------------|
| Pop / uplift         | I–V–vi–IV                        | 4-bar; rotate starting degree                 |
| Pop / introspective  | vi–IV–I–V                        | 4-bar minor-center feel                       |
| Classical cadence    | I–IV–V–I                         | 4-bar; cadential 6/4 variant                  |
| Classical authentic  | ii6–V7–I                         | 3-chord cadence placed over 4 bars            |
| Jazz turnaround      | Imaj7–vi7–ii7–V7                 | 4-bar, extensions                             |
| Jazz ii–V–I          | ii7–V7–Imaj7                     | 4 or 8 bars with hold                         |
| Minor jazz           | iiø7–V7alt–i(mMaj7/min7)         | 4-bar minor                                   |
| 12-bar blues         | I7 / IV7 / V7                    | 12-bar special case; cannot truncate silently |
| Minor blues          | i7–iv7–V7                        | 12-bar variation                              |
| Romantic / cinematic | i–VI–III–VII                     | 4-bar minor                                   |
| Suspense pedal       | i–♭II–i–V                        | Dissonant/Phrygian color                      |
| Heroic               | i–VI–III–VII → i or I–♭VII–IV–I  | Choose tonal context                          |
| Whimsical            | I–iii–IV–V                       | Light movement                                |
| Modal folk           | i–♭VII–♭VI–♭VII                  | Aeolian/modal                                 |
| Neo-soul             | Imaj9–iii7–vi9–ii9–V13           | 8-bar voice-led                               |
| Industrial ambient   | i pedal + upper-structure shifts | Sparse and nonfunctional                      |

Musical engine requirement: represent progressions as scale degrees/functions plus explicit alterations and quality, not hard-coded C-major note lists. Respect harmonic minor leading tones, modal mixtures, chromatic chords, enharmonic spelling and transposing instruments. Major/minor labels are insufficient for raga/makam traditions; offer configurable pitch collections and tuning, without asserting western chord rules apply universally.

## 9.3 Data/algorithms

- Core types: PitchClass/TuningSystem, KeySignature/Mode, ChordSymbol, ChordToneSet, Voicing, ChordEvent(start_tick,duration_tick,velocity), ProgressionPattern, AuditionVariant.

- Transposition: retain Roman-numeral functions across keys; derive displayed chord names with musically appropriate accidentals; use pitch-class math and a spelling layer separately.

- Voice leading: deterministic algorithm minimizes total semitone movement within instrument range and avoids unwanted crossings, with user-controlled inversions/voicing locks.

- Quantize at DAW musical ticks (PPQ) with time-signature-aware bar arithmetic; tempo changes do not move MIDI event bar positions.

# 10. Ostinato Creator

## 10.1 Modal generator interface

- Inputs: target track/instrument, project key/mode or custom scale, optional selected progression to follow, meter, tempo, start bar, length in bars (explicitly 16 supported), note range, density, rhythm grid, rests, accents, swing, pitch contour, interval limit, articulation and velocity range.

- Style presets: urgent string spiccato, rolling piano arpeggio, pulsing synth, low brass ostinato, mallet pattern, pizzicato comedy figure, minimalist repetition, syncopated percussion, cinematic hybrid, culturally curated patterns where appropriate.

- Generate multiple unique variations with persistent variant IDs and deterministic random seeds. Display piano-roll preview and A/B/C cards; loop audition each variant through selected instrument (with metronome/tempo option).

- Variation controls: change one parameter at a time (rhythm, pitch, accents, register, density, articulation); mutate/lock chosen notes, beats, contours and chord tones; add call/response or evolving 4-bar segments across 16 bars.

- Chord-follow options: root-only, chord tones, scale tones, passing tones, pedal, chromatic tension, no-harmony/drum pitch mode. Adapt notes at chord boundaries without changing target duration.

- Send to Track: insert MIDI events at exact start bar and duration into current track or new track; optional create separate take lane; collision policy (replace/merge/new lane) must be explicit; undo as one transaction.

## 10.2 Generation and preview constraints

- Must produce exactly N bars for arbitrary supported time signatures (e.g., 3/4, 4/4, 5/4, 7/8), using project ticks and no stray note-offs.

- Default generated note events honor playable range, articulation compatibility and instrument polyphony; percussion mode uses drum mappings instead of pitched-key logic.

- Use seeded generation for reproducibility and variant recall; asynchronous generation and playback must not block the audio thread.

- Audition starts/stops immediately, handles MIDI note-offs and transport state safely, and does not render destructive audio or write into project until Send to Track.

# 11. UI specification and integration contracts

| Surface              | Controls                                                             | Behavior                                                |
|----------------------|----------------------------------------------------------------------|---------------------------------------------------------|
| Template browser     | Search, categories, tags, filters, preview, create/add               | Resizable window; keyboard navigation; recent/favorites |
| Track inspector      | Instrument, technique map, notes, mapping warnings                   | Allows relabel/remap/disable articulations              |
| Chord pop-up         | Key, progression cards, chord matrix, inversions, keyboard, playback | 4/8-bar placement and track selection                   |
| Ostinato pop-up      | Length, meter, groove, style, generation, A/B/C audition             | 16-bar variant generation and Send to Track             |
| Piano roll           | Regions, notes, CC, articulation markers                             | Generated MIDI editable like hand-entered MIDI          |
| Detached mixer       | Instrument channels, section buses, inserts, sends                   | Template routing appears and remains synchronized       |
| Temp/reference track | Separate reference audio lane; mute/solo/toggle                      | Never altered by template selection or auditions        |

# 12. Proposed internal services and APIs

Implementation-neutral interfaces (names illustrative):

TemplateRegistry.list(filter) / get(templateId, version) / validate(spec) / instantiate(projectId, options) / saveUserTemplate(snapshot); InstrumentResolver.resolve(standardInstrumentId, techniques, installedPlugins); HarmonyEngine.transpose(progression, key, tuning) / voice(progression, options) / renderMidi(...); OstinatoEngine.generate(seed, options, harmony) / vary(variantId, locks) / renderMidi(...); AuditionService.preview(midi, instrumentId, loop) / stop(); SequencerTransaction.insertMidi(targetTrack, startTick, notes, collisionPolicy) / undo().

Keep composition logic as testable pure functions where possible. UI calls may run worker jobs but must never run file/database/AI work on real-time audio callback threads. Do not rely on online AI generation to produce the built-in palettes or basic variations.

# 13. Data safety, licensing and cultural quality

- Shipping content must be original, public-domain where applicable, or properly licensed. A historical composer’s name is metadata, not a license to bundle protected performances or modern editions.

- Use neutral production-language alternatives to living artist branded presets; do not clone proprietary recordings or sampled instrument patches.

- Regional/traditional packs should include culture/subregion, tradition, instrument provenance, tuning assumptions, reviewer/source fields and a “cinematic approximation” marker as relevant. Seek knowledgeable review of culturally specific entries.

- Validate malformed template files, third-party plugin absence, missing patch presets, MIDI out-of-range notes and incompatible project versions.

# 14. Build order and milestones

| Phase                         | Deliverable                                                           | Exit gate                                       |
|-------------------------------|-----------------------------------------------------------------------|-------------------------------------------------|
| P0 — foundation               | Schema, registry, instrument IDs, sample/placeholder resolver         | Built-in packs validate and instantiate offline |
| P1 — initial template catalog | Narrative, modern, regional and period/composer curated packs         | Correct track trees and sections with undo      |
| P2 — chord explorer           | Key/mode engine, progressions, voicing/inversions, preview, insertion | 4/8-bar MIDI matches displayed harmony          |
| P3 — ostinato creator         | Seeded generator, 16-bar preview and insertion                        | Variants reproduce and respect meter/scale      |
| P4 — polish                   | Custom/save/import/export, mapper warnings, favorites, tests          | Cross-platform regression passes                |

# 15. Coding-agent acceptance criteria

7.  Horror template creates labeled low-string technique tracks, woodwinds, atmospheric textures, section bus routing and editable articulation data—even if instrument plugins are missing.

8.  Comedy template includes pizzicato strings, oboe, bassoon and pitched percussion; Whimsical, Romance, Sad, Heroic and Suspense presets are discoverable as distinct options.

9.  Ireland, Japanese traditional, Chinese traditional, three or more clearly differentiated African regional traditions, German folk and early Germanic historical/reconstruction presets are searchable separately.

10. Bach, Vivaldi, Mozart and Beethoven composer presets are distinctly configured and labeled; at least one musical-era variant is available where relevant.

11. Blues, several jazz eras, multiple rock eras, industrial electronic and minimalist dark cinematic hybrid presets are discoverable.

12. Selecting a 4-bar I–V–vi–IV progression in G major displays G–D–Em–C and writes the expected MIDI pitches at the specified bar, following the selected voice-leading settings.

13. A user can click through chord inversions, 7ths and 9ths, hear each without writing to the arrangement, and insert exactly 8 bars as one undoable edit.

14. A 16-bar string ostinato can produce at least three auditionable variations, be sent to a selected track, and be edited immediately in the piano roll.

15. Template import and project merges never erase existing cues, mixer buses, or the reference/temp audio track without explicit user action.

16. Offline macOS and Windows builds produce equivalent saved-project results; broken plugin mappings yield visible warnings and functional MIDI placeholders.

# 16. Representative test cases

| ID  | Test                                           | Expected                                        |
|-----|------------------------------------------------|-------------------------------------------------|
| T01 | Load Horror without any orchestral plugins     | Named placeholders; no crash                    |
| T02 | Load Comedy with only generic GM synth         | Instrument roles retained; mapped fallback      |
| T03 | Open Bach → concerto variant                   | Strings/continuo and suitable winds if selected |
| T04 | Transpose minor iiø–Valt–i to E minor          | Correct note spelling and chord quality         |
| T05 | Preview chords while temp audio track is muted | Temp audio remains unchanged                    |
| T06 | Generate 16 bars in 7/8                        | Exact meter-correct MIDI event duration         |
| T07 | Regenerate with same seed/options              | Identical ostinato output                       |
| T08 | Send MIDI to occupied region                   | Replace/merge/new lane prompt respected         |
| T09 | Undo template insert                           | All newly created tracks/buses removed as unit  |
| T10 | Import custom culturally curated pack          | Metadata validation and warnings                |

# 17. Product decisions for implementation

Defaults proposed where no earlier requirement specified: local/offline core library; 4/8-bar chord insertion with 12-bar blues exception; 16-bar ostinato default; three audition variants; non-destructive project merge; no unlicensed audio bundled; built-in templates immutable, custom copies editable. Treat these as implementation defaults, not as previously user-approved UX details.

Definition of done: a composer can choose a scoring approach, instantiate appropriately named tracks and articulations, hear and place key-correct chord progressions, generate and audition a 16-bar ostinato, and mix the resulting project using the existing VC Composer 2 workstation without leaving the app.
