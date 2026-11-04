/*
 * Copyright 2026 Feralthedogg
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * See LICENSE.
 */

import type { ReactNode } from 'react'
import { Stage } from '../../../third_party/stageshow/kit/stage'
import type { Act } from '../../../third_party/stageshow/kit/act'
import { States } from '../../../third_party/stageshow/kit/motion/States'
import { Spinner } from '../../../third_party/stageshow/kit/parts/Spinner'
import './park.css'

// A system story, no cursor: what llam_read() does to a scheduler. The code on
// the left is a real connection handler; the trace on the right is the runtime
// as that handler runs. conn 1 reaches llam_read, parks on the I/O backend, and
// its worker runs other tasks with no hole in its bar. When the fd is readable
// conn 1 continues on the next line, on a different worker.
//
// What makes it read:
// - One lit color: conn 1, in the code's header, the chip and its bars. Every
//   other task is gray and wordless, so the eye follows one object.
// - The trace is one track on one linear clock. The bars are already laid out
//   and the now line reveals them, so nothing per frame moves but one transform.
// - The handler is a for(;;), so the loop has a real cause: it goes round twice
//   (worker 0 to worker 1, then back), and the second time is the short one.
// - The status changes on one pill, on the line that caused it.

const P = 14000 // one full turn of the loop, ms
const V = 30 / 1000 // trace speed, px per ms

const PARK_A = 1800
const WAKE_A = 5600
const PARK_B = 9300
const WAKE_B = 11800

type Chip = 'w0' | 'io' | 'w1'
type Pill = 'off' | 'parked' | 'done'
type S = { t: number; run: boolean; hl: number; pill: Pill; chip: Chip; note: boolean }

const LINES = [
  'static void handle_conn(void *arg) {',
  '    llam_fd_t fd = (llam_fd_t)(intptr_t)arg;',
  '    char buf[4096];',
  '',
  '    for (;;) {',
  '        ssize_t n = llam_read(fd, buf, sizeof buf);',
  '        if (n <= 0) break;',
  '',
  '        size_t sent = 0;',
  '        if (llam_write_all(fd, buf, n, &sent)) break;',
  '    }',
  '    llam_close(fd);',
  '}',
]
const FOR = 4
const READ = 5
const IF = 6
const WRITE = 9
// The phone column shows only the loop, one indent shallower, so the longest
// line still reads at 11.5px
const COMPACT_FIRST = 4
const COMPACT_LAST = 10

const initial: S = { t: 0, run: false, hl: FOR, pill: 'off', chip: 'w0', note: false }
const poster: S = { t: 7300, run: false, hl: WRITE, pill: 'done', chip: 'w1', note: false }

async function script(a: Act<S>) {
  // The resting track has to paint once, or there is no transition to run
  await a.wait(40)
  a.put({ t: P, run: true })
  await a.wait(700)
  a.mark('read')
  a.put({ hl: READ })
  await a.wait(PARK_A - 700)
  a.mark('park')
  a.put({ pill: 'parked', chip: 'io' })
  await a.wait(WAKE_A - 400 - PARK_A)
  a.put({ note: true })
  await a.wait(400)
  a.mark('wake')
  a.put({ chip: 'w1', note: false })
  await a.wait(700)
  a.put({ pill: 'done' })
  await a.wait(400)
  a.put({ hl: IF })
  await a.wait(600)
  a.mark('write')
  a.put({ hl: WRITE })
  await a.wait(1300)
  // Round the loop: the old result goes with the line that made it
  a.put({ hl: FOR, pill: 'off' })
  await a.wait(300)
  a.put({ hl: READ })
  await a.wait(PARK_B - 8900)
  a.mark('park-2')
  a.put({ pill: 'parked', chip: 'io' })
  await a.wait(WAKE_B - 400 - PARK_B)
  a.put({ note: true })
  await a.wait(400)
  a.put({ chip: 'w0', note: false })
  await a.wait(700)
  a.put({ pill: 'done' })
  await a.wait(400)
  a.put({ hl: IF })
  await a.wait(400)
  a.put({ hl: WRITE })
  await a.wait(500)
  a.put({ hl: FOR, pill: 'off' })
  await a.wait(200)
  // The pattern repeats every P, so this jump lands on an identical picture
  await a.snap({ t: 0, run: false })
}

type Seg = { from: number; to: number; tone?: number; lit?: boolean }

// One period of each lane. Every worker is busy for the whole period.
const W0: Seg[] = [
  { from: WAKE_B, to: P + PARK_A, lit: true },
  { from: PARK_A, to: 5000, tone: 2 },
  { from: 5000, to: 8400, tone: 1 },
  { from: 8400, to: WAKE_B, tone: 3 },
]
const W1: Seg[] = [
  { from: WAKE_A, to: PARK_B, lit: true },
  { from: PARK_B, to: 11900, tone: 1 },
  { from: 11900, to: P + 2600, tone: 3 },
  { from: 2600, to: WAKE_A, tone: 2 },
]
const W2: Seg[] = [
  { from: 0, to: 3400, tone: 2 },
  { from: 3400, to: 6100, tone: 1 },
  { from: 6100, to: 9900, tone: 3 },
  { from: 9900, to: 12600, tone: 1 },
  { from: 12600, to: P, tone: 2 },
]
const IO: Seg[] = [
  { from: PARK_A, to: WAKE_A, lit: true },
  { from: PARK_B, to: WAKE_B, lit: true },
]

function Track({ segs, wait }: { segs: Seg[]; wait?: boolean }) {
  const out: ReactNode[] = []
  for (const k of [-2, -1, 0, 1]) {
    segs.forEach((s, i) => {
      const from = s.from + k * P
      const to = s.to + k * P
      out.push(
        <i
          key={`${k}-${i}`}
          className="lp-seg"
          data-lit={s.lit || undefined}
          data-wait={wait || undefined}
          data-tone={s.tone}
          style={{ left: from * V, width: (to - from) * V - 2 }}
        />,
      )
    })
  }
  return <>{out}</>
}

// conn 1's hops, drawn between lanes on the same track as the bars
const HOPS = [
  { at: PARK_A, gap: 0 },
  { at: WAKE_A, gap: 1 },
  { at: PARK_B, gap: 1 },
  { at: WAKE_B, gap: 0 },
]

const KEYWORD = /^(static|void|char|for|if|break|sizeof)$/
const TYPE = /^(llam_fd_t|ssize_t|size_t|intptr_t)$/

function tokens(line: string) {
  const out: ReactNode[] = []
  const re = /([A-Za-z_][\w]*)(\s*\()?|(\d+)/g
  let last = 0
  let m: RegExpExecArray | null
  let key = 0
  while ((m = re.exec(line))) {
    if (m.index > last) out.push(line.slice(last, m.index))
    const [text, word, call, num] = m
    if (num) out.push(<span key={key++} className="lp-num">{text}</span>)
    else if (KEYWORD.test(word)) out.push(<span key={key++} className="lp-key">{text}</span>)
    else if (TYPE.test(word)) out.push(<span key={key++} className="lp-type">{text}</span>)
    else if (call) out.push(<span key={key++} className="lp-fn">{word}</span>, call)
    else out.push(text)
    last = m.index + text.length
  }
  if (last < line.length) out.push(line.slice(last))
  return out
}

function Status({ pill }: { pill: Pill }) {
  return (
    <span className="lp-pill" data-on={pill !== 'off' || undefined} data-tone={pill}>
      <States
        value={pill === 'done' ? 'done' : 'parked'}
        states={{
          parked: (
            <span className="lp-pill__row">
              <Spinner size={10} />
              parked
            </span>
          ),
          done: <span className="lp-pill__row">n = 512</span>,
        }}
      />
    </span>
  )
}

export default function LlamPark() {
  return (
    <Stage<S>
      width={1120}
      height={400}
      compact={{ below: 760, width: 358, height: 492 }}
      theme="dark"
      initial={initial}
      poster={poster}
      script={script}
      className="lp"
      tokens={{
        '--ss-font': 'Inter, system-ui, sans-serif',
        '--ss-mono': '"JetBrains Mono", ui-monospace, monospace',
        '--ss-accent': '#4D5BFF',
      }}
    >
      {(s, _rect, layout) => {
        const compact = layout === 'compact'
        const first = compact ? COMPACT_FIRST : 0
        const last = compact ? COMPACT_LAST : LINES.length - 1
        const shown = LINES.slice(first, last + 1)
        const lh = compact ? 20 : 24
        // Lane geometry: worker 0, the I/O backend between the workers so conn 1
        // never crosses a lane, then worker 1 and a worker that only keeps busy
        const w = compact ? 34 : 48
        const io = compact ? 22 : 28
        const gap = compact ? 12 : 20
        const y = { w0: 0, io: w + gap, w1: w + gap + io + gap, w2: w + gap + io + gap + w + gap }
        const lanes = y.w2 + w
        const chipH = compact ? 24 : 30
        const chipY = { w0: (w - chipH) / 2, io: y.io + (io - chipH) / 2, w1: y.w1 + (w - chipH) / 2 }
        return (
          <div className="lp-panel">
            <section className="lp-code">
              <div className="lp-bar">
                <span className="lp-bar__title">echo.c</span>
                <span className="lp-tag">conn 1</span>
              </div>
              <div className="lp-lines" style={{ ['--lh' as string]: `${lh}px` }}>
                <i className="lp-hl" style={{ transform: `translateY(${(s.hl - first) * lh}px)` }} />
                {shown.map((text, i) => {
                  const n = first + i
                  const line = compact ? text.slice(4) : text
                  return (
                    <div className="lp-line" key={n}>
                      {!compact && <span className="lp-gutter">{n + 1}</span>}
                      <code>{tokens(line)}</code>
                      {/* On the phone the read line fills the column, so the status
                          takes the short line under it, the one that reads n */}
                      {n === (compact ? IF : READ) && <Status pill={s.pill} />}
                    </div>
                  )
                })}
              </div>
            </section>

            <section className="lp-trace">
              <div className="lp-bar">
                <span className="lp-bar__title">Scheduler</span>
                <span className="lp-now">now</span>
              </div>
              <div className="lp-lanes" style={{ height: lanes }}>
                <span className="lp-name" style={{ top: y.w0, height: w }}>worker 0</span>
                <span className="lp-name lp-name--mono" style={{ top: y.io, height: io }}>io_uring</span>
                <span className="lp-name" style={{ top: y.w1, height: w }}>worker 1</span>
                <span className="lp-name" style={{ top: y.w2, height: w }}>worker 2</span>

                <div className="lp-view">
                  <div className="lp-track" data-run={s.run || undefined} data-snap style={{ transform: `translateX(${-s.t * V}px)` }}>
                    <div className="lp-lane" style={{ top: y.w0, height: w }}><Track segs={W0} /></div>
                    <div className="lp-lane" style={{ top: y.io, height: io }}><Track segs={IO} wait /></div>
                    <div className="lp-lane" style={{ top: y.w1, height: w }}><Track segs={W1} /></div>
                    <div className="lp-lane" style={{ top: y.w2, height: w }}><Track segs={W2} /></div>
                    {[-2, -1, 0, 1].flatMap((k) =>
                      HOPS.map((h, i) => (
                        <i
                          key={`${k}-${i}`}
                          className="lp-hop"
                          style={{
                            left: (h.at + k * P) * V - 1,
                            top: h.gap === 0 ? y.w0 + w : y.io + io,
                            height: gap,
                          }}
                        />
                      )),
                    )}
                  </div>
                </div>
                <i className="lp-nowline" />

                <div className="lp-slots">
                  <span className="lp-chip lp-chip--idle" data-on={s.chip !== 'w0' || undefined} style={{ top: chipY.w0, height: chipH }}><i /></span>
                  <span className="lp-chip lp-chip--idle" data-on={s.chip !== 'w1' || undefined} style={{ top: chipY.w1, height: chipH }}><i /></span>
                  <span className="lp-chip lp-chip--idle" data-on style={{ top: y.w2 + (w - chipH) / 2, height: chipH }}><i /></span>
                  <span className="lp-chip lp-chip--lit" style={{ height: chipH, transform: `translateY(${chipY[s.chip]}px)` }}>conn 1</span>
                </div>
                <span className="lp-note" data-on={s.note || undefined} style={{ top: y.io, height: io }}>fd readable</span>
              </div>
              <div className="lp-foot">
                <span>Workers blocked while conn 1 waits</span>
                <b>0</b>
              </div>
            </section>
          </div>
        )
      }}
    </Stage>
  )
}
