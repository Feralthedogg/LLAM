<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
See LICENSES/Apache-2.0.txt.
-->

# LLAM 비동기 선점 아키텍처 설계

작성일: 2026-09-22. 대상: 로컬 `main`의 통합된 작업 상태.
상태: **RESEARCH COMPLETE — 아키텍처 설계 완료, 구현·지원 승인 아님.**
실제 웹 ChatGPT `6 Pro`와 2회 검토했고, 최종 수정 규약을 아래에 반영했다.
런타임 구현 변경 없음. Native backend·기존 TSan 문제·성능 검증은 implementation-open이다.

## 1. 요청과 설계 합격 조건

목표는 사용자 연산에 수동 `yield`를 넣지 않아도 다른 관리 태스크와 I/O가
진행하는 C11 stackful N:M 런타임이다. ‘아무 C 코드나 어떤 명령에서도 안전하게
이동·취소 가능’과 ‘일반 OS에서 절대 지연 상한 보장’은 별개의 주장이고 채택하지 않는다.
안전 조건을 숨기지 않으면서 실제 no-poll 연산을 중단할 수 있어야 한다.

이번 산출물은 설계와 검증 계획이다. 구현 완료, race-free 증명, native 플랫폼 통과,
성능 향상을 주장하지 않는다. 상세 출처·읽은 범위·실험은 [evidence.md](evidence.md)에 있다.

| 합격 조건 | 요구 산출물 |
| --- | --- |
| AC1 | 어떤 실행에 비동기 선점·공정성·진행을 보장하며 무엇을 제외하는지 정의 |
| AC2 | G/M/P 권위, capture→park→resume, publication·수명·memory ordering 계약 |
| AC3 | OS/ISA/컴파일러/ABI/foreign 호출의 명시적 capability 및 실패 계약 |
| AC4 | 기존 코드에 연결되는 구현 순서, 회귀·스트레스·지연 측정 gate |
| AC5 | 실제 최신 Pro 협업과 결론을 바꿀 수 있는 주장에 대한 직접 검증 |

## 2. 출발점: 이미 있는 것과 아직 없는 것

HEAD는 `34dcc74a7e3cbb584967f6bf5f7816673ed5169b`지만, 상당한 미커밋 구현이 통합되어 있다.
HEAD만을 baseline으로 부르지 않는다. [근거 파일](evidence.md)에 실제 파일 해시를 기록했다.

| 현재 구현 | 설계상 남은 차이 |
| --- | --- |
| worker-local active G, incarnation/run generation, legacy execution lease와 P ticket | 일반 M↔P 실행권 이전 및 저장 완료 증명은 별도 정합성 계약 필요 |
| P-owned queues/timers/allocator/wake/logical I/O route | `current == NULL`만으로 P payload quiescence를 판단해서는 안 됨 |
| foreign enter/leave와 helper/redirect | 임의 opaque C를 탐지·이동하는 기능이 아님; native 실행 수명과 자원 한도 필요 |
| Linux x86-64 async 실험 | deterministic 1P, process당 1 runtime, primary M, singleton resume PC/capture 제한 |
| compiler metadata와 private 등록 | 메모리 접근·호출을 거절하는 작은 annotated 함수만 처리; 자동 로딩·완전 coverage 없음 |
| AUTO/STRICT 및 내부 capability | AUTO는 여전히 보통 cooperative; STRICT는 diagnostic이지 async-required 아님 |
| private Magic2 policy/tile 경로 | 커널 선택과 중단 안전성은 다른 문제; 자동 경로는 주로 benchmark에서 사용 |

기존 `llam_yield()`는 ready enqueue 뒤에 최종 context/errno 저장을 한다.
현재 모든 consumer를 막는 exclusion이 있는지 확인해야 하며, 더 넓은 선점을 넣기 전에
`저장 완료 → 이전 실행자 완전 이탈 → 다른 실행자에게 공개` 불변식을 입증해야 한다.
이 관찰만으로 기존 TSan 실패 원인을 단정하지 않는다.

### 선행 차단 gate

기존 통합 TSan 로그의 ownerless shutdown cleanup timeout/segfault는 미해결이다.
이 실패를 줄인 직접 회귀 테스트와 수명/전환 정합성 확인이 기능 확장의 선행 조건이다.
일반 테스트와 ASan 통과가 이 실패를 무효화하지 않는다.

## 3. 독립 확인으로 확정된 제약

1. GPR만 저장해서는 부족하다. SIMD/FP, 활성화된 확장 상태, 신호 마스크와 native 상태의
   범위를 플랫폼별로 정의해야 한다. Linux [XSTATE](https://docs.kernel.org/arch/x86/xstate.html),
   [SVE](https://docs.kernel.org/arch/arm64/sve.html), [SME](https://docs.kernel.org/arch/arm64/sme.html)가 이를 구체화한다.
2. 외부 API가 thread를 멈출 수 있다는 사실은 그 지점에서 scheduler를 실행해도 안전하다는 뜻이 아니다.
   [Go의 검사](https://go.dev/src/runtime/preempt.go)와
   [Windows 제약](https://learn.microsoft.com/en-us/windows/win32/procthread/suspending-thread-execution)을 함께 적용해야 한다.
3. native TLS는 G-local 저장소가 아니다. 실제 Clang compile-only 실험에서 opaque C 호출
   전후 TLS 주소가 재사용됐다. [입력](artifacts/tls_codegen_probe.c),
   [arm64 IR](artifacts/tls_codegen_probe.ll), [x86-64 IR](artifacts/tls_codegen_probe_linux_x86_64.ll).
   OS 스레드 이동 뒤 TLS 포인터만 교체하는 설계는 이 조건을 해결하지 못한다.
4. signal handler에서 돌아온 다음 trampoline을 실행해도, 원래 코드가 보유한 libc/loader/native
   lock이나 TLS 재진입 문제가 자동으로 사라지지 않는다.
5. 선점과 취소는 다르다. 중단한 연산을 재개할 수 있다는 사실은 부분 쓰기를 롤백하거나
   C stack을 임의로 폐기할 권한이 아니다. 기존 LLAM stop/cancel 의미를 유지한다.
6. interrupt만 빨라져도 I/O·큐·scheduler가 병목이면 전체 응답 지연은 개선되지 않는다.
   [NSDI 2025의 동일 런타임 비교](https://www.usenix.org/system/files/nsdi25-guo.pdf)가 이 분리를 뒷받침한다.

## 4. 채택 구조와 명시적으로 버리는 대안

Pro 1차 검토는 다음 조합을 권고했고, 2차 검토는 수정된 수명·복원·origin-M 규약을 포함한
아키텍처를 승인했다. 아래는 실제 LLAM 경계에 대응시킨 최종 설계이며, 이미 구현됐다는 뜻이 아니다.

**관리 코드에는 실제 비동기 G 선점, 위험한 짧은 구간에는 컴파일러가 생성한 진행 경계,
외부 코드에는 native-thread 실행 보존과 제한된 보상**을 사용한다.

| 실행 영역 | 선택 방식 | 중단·재개 계약 |
| --- | --- | --- |
| Legacy compatible | 기존 binary/metadata 없음 | 기존 cooperative 의미 보존. async 보장으로 표시하지 않음 |
| Certified managed, origin-M | 최종 binary 인증 + 동적 안전 영역 + backend capability | 수동 yield 없는 CPU loop도 async 중단. 처음에는 원래 M에서만 재개 |
| Certified managed, migratable | 위 조건 + 별도 migration ABI/TLS/call-closure 인증 | 호환 CPU-state를 가진 다른 M에서도 재개. 후속 단계 |
| Compiler-covered | 자동 삽입한 경계가 모든 장시간 경로를 덮음 | 사용자 수동 yield 불필요. 기계적 방식은 cooperative이므로 async capture와 구별 |
| Native exclusive island | native identity/미인증 library/장시간 foreign 영역 | activation 동안 같은 OS thread, 다른 G multiplex 금지. OS 선점에 맡기며 관리 P와 용량 분리 |
| Runtime critical | P payload 조작·context 전환·loader publication | 짧은 disable scope. 지연됐다고 강제 capture하지 않음 |

이 선택은 최종적으로 compiler/runtime가 자동 수행한다. 사용자가 Magic2 plan API나
수동 safe-range 등록 API를 호출하게 하는 방향이 아니다. 다만 **자동 판정 불가를 안전으로 간주하지 않는다.**
기존 프로그램은 묵시적으로 승격하지 않고, 인증된 빌드/실행 profile에 강한 계약을 적용한다.

Capture-safe, schedule-safe, migrate-safe, cancel-safe는 서로 다른 허가다.
일반적인 managed async 선점에는 앞의 두 개가 필요하며, 뒤의 두 개는 따라오지 않는다.
순수 leaf PC도 native mutex를 잡은 caller에서 호출됐다면 schedule-safe라고 볼 수 없다.
잠금 없는 동일 P 구간이 cooperative 상호배제를 전제로 했다면 opt-in 전 소스 수정이 필요하다.

선택하지 않는 기본안:

- 모든 PC에서 snapshot 후 다른 G 실행: native lock/TLS/재진입 계약을 모른다.
- timeout만 보고 P epoch를 바꾸기: 진행 중 owner-side 작업을 중단시키지 못한다.
- 무제한 helper 생성: 메모리/스레드 고갈과 oversubscription을 만든다.
- 모든 경로에 polling만 추가: 미계측 코드를 해결하지 못하고 실제 instruction-level async 목표를 대체한다.
- UINTR/Dune/custom kernel를 필수로 요구: portable library 범위를 벗어난다. 별도 성능 실험 후보로만 남긴다.

## 5. G/M/P 권위와 continuation 수명

### 5.1 자료구조의 논리적 책임

아래 필드는 설계명이며 아직 공개 C 선언이 아니다.

| 객체 | 소유 상태 | 권위가 아닌 것 |
| --- | --- | --- |
| G | `context_kind={COOP,ASYNC}`, capsule, suspend/resume sequence, stack/code lifetime refs, origin-M 제약, task errno/FP 환경 | M의 TLS/현재 OS thread를 소유하지 않음 |
| M | scheduler continuation, binding incarnation, immutable dispatch descriptor, 요청 mailbox, altstack, landing stack, restore arena, 진행 중 restore record | associated P 포인터만으로 P 소유권을 얻지 못함 |
| P | queues/timers/allocator/logical I/O route, 현재 단일 execution lease의 검증된 projection | G snapshot이나 native syscall continuation을 소유하지 않음 |
| Runtime | admission·capture-memory·native-worker 예산, live references, shutdown/drain 상태 | peer runtime의 stop·queue·G를 제어하지 않음 |
| Process broker | 예약 signal과 native binding의 안전한 공존/retirement | singleton capture buffer, singleton resume PC, 전역 G scheduler를 만들지 않음 |

기존 legacy lease를 한 번에 별도 atomic owner로 교체하지 않는다. R0–R3에서는 기존
`opaque_lock → shard->lock` 순서와 단일 coordinator를 유지한다. 일반 M/P pool로 확장할 때도
lease와 P projection을 서로 다른 writer가 독립 갱신하는 중간 상태를 허용하지 않는다.

식별자는 서로 다른 ABA를 구별한다: runtime instance/run, G lifetime/run,
M incarnation/dispatch, P lease/generation, capture request, code generation.
하나의 epoch가 모든 수명을 대체하지 않는다. wraparound는 기존 incarnation과 재사용할 수 없으며
포화 시 새 admission을 거절하는 실패 경로가 필요하다.

### 5.2 핵심 불변식

1. 한 G의 user instruction과 유효한 resume claim은 동시에 최대 하나다.
2. `READY` consumer가 G를 얻을 때 원래 context 저장과 old executor의 stack 이탈이 끝났다.
3. `CAPTURE_ACK`는 register 저장 완료이며 `QUIESCENT_ACK`나 lease handback ACK가 아니다.
4. P ticket 변경은 old-owner의 P-local borrow/임계 작업 종료 이후에만 가능하다.
5. Descriptor를 읽는 handler보다 G/M/runtime/code/storage pin이 먼저 존재한다.
6. 살아 있는 capsule에는 altstack·landing stack 내부의 장기 차용 pointer가 없다.
7. Signal, wake, cancel, completion의 중복은 enqueue/resume를 중복 실행시키지 않는다.
8. Restore transition은 handler 관점에서 사용자 실행과 혼합된 반쪽 상태를 노출하지 않는다.
9. Stop·cancel 요청과 `active_g == NULL`은 stack/code 해제 증거가 아니다.
10. 같은 process의 불량 native 코드를 격리한다는 보장은 없다. 적대적 코드 격리는 broker/process 경계의 역할이다.

### 5.3 저장공간과 admission

장기 capsule은 G 소유다. **초기 구현은 G당 두 capsule bank**를 둔다.
각 bank의 payload는 capture generation 동안 immutable이고 lifecycle header만 atomic하게 바뀐다.
M capture mailbox/storage와 M restore arena는 별개다. M binding당 미확인 restore attempt는 최대 하나다.
M당 한 buffer만 두면 여러 G가 중단된 경우 덮어쓰거나 다른 선점을 막는다.
첫 async dispatch 전에 두 bank에 지원 ISA의 최대 허용 context size를 확보하고 prefault 가능한 정책을 적용한다.
handler에서 malloc/mmap, stack 증가, loader lookup을 하지 않는다.

예산은 대략 `활성 async G 수 × (2 × capsule 한도 + headers) + M 수 × (mailbox/storage + altstack + landing + restore arena)`로 계산한다.
G stack은 별도다. 활성 CPU extension으로 크기가 바뀌면 기존 허용량과 비교하고 재협상 전 admission을 막는다.
이미 보장 모드에 받아들인 G가 선점 순간에 처음 메모리를 요구하는 구조는 금지한다.
자원 부족은 실행 전 `ENOMEM/EAGAIN` 계열 실패 또는 명시된 compatible 경로로 처리하고,
required 계약에서 조용히 선점 보장을 낮추지 않는다. 정확한 공개 오류 mapping은 ABI 단계에서 고정한다.

## 6. 요청 → capture → scheduler commit → resume

### 6.1 Dispatch와 요청

Scheduler가 G/runtime/code/storage reference를 획득하고 context kind 및 실행 영역을 확인한다.
M의 signal-visible descriptor는 완성한 뒤 release로 공개한다. 여러 plain field를 동시에 덮어쓰고
generation만 전후 비교하는 방식은 C data race를 해결하지 못한다. immutable record 또는
전 필드 atomic 규약과 명시된 reclamation을 선택한다.

Watchdog은 현재 dispatch identity에 대한 pending request를 기록한 뒤 native binding을 pin하고
검증된 M에 doorbell을 보낸다. Signal에는 정확한 G identity가 실린 것으로 가정하지 않는다.
오래된 signal은 현재 tuple 검증을 통과하지 못한다. 요청을 중복 병합하되 unsafe deferral 뒤에는
bounded retry가 가능해야 한다. OS가 동일 신호를 합칠 수 있으므로 signal 수를 request 완료 수로 세지 않는다.

요청 상태와 실제 실행 상태를 구별한다.

| 상태 | G를 ready consumer가 실행할 수 있는가 | 소유자 |
| --- | --- | --- |
| RUNNING / request pending | 아니오 | 현재 M |
| CAPTURING | 아니오 | handler claim을 가진 현재 M |
| LANDING | 아니오 | M의 전이 경로 |
| READY_ASYNC | 예, unique claim 필요 | queue + G capsule |
| RESUMING / RESUME_UNCONFIRMED | 다른 consumer는 불가 | claim과 restore record를 가진 M |
| RUNNING_USER | 아니오 | 검증된 새 dispatch epoch |

### 6.2 Handler의 허용 동작

유한 횟수 atomic read/claim, 검증된 범위 조회, 사전 확보된 저장공간 복사,
OS return-context 편집만 허용한다. lock·allocation·일반 logging·symbol lookup·yield는 금지한다.
seqlock writer가 끝날 때까지 기다리는 loop도 금지한다. handler가 중단한 writer 자신을 기다릴 수 있다.

검증 항목은 current G/P/epoch만이 아니다. PC와 SP/stack 범위, 실제 활성 ISA 상태,
동적 foreign/native-lock/host-signal callback 영역, runtime critical/preempt-disable depth,
capture storage readiness, capsule 상태를 함께 본다. `MANAGED_SAFE` 이름만으로 통과시키지 않는다.
generated object에서 TLS resolver, compiler helper, sanitizer/profiling call이 생기지 않는지도 감사한다.

신호로 전달된 원래 context를 저장한다. handler의 현재 register를 저장하지 않는다.
GPR/flags/PC/SP와 필요한 FP/SIMD/확장 payload를 독립 capsule로 복사하며,
OS frame 내부 pointer를 장기 보관하지 않는다. 알 수 없는 필수 state는 저장 일부 성공으로 처리하지 않는다.

### 6.3 Landing과 quiescent commit

OS signal return은 M 전용 landing stack으로 진입한다. M당 OS altstack 설치 권위는 하나이며
fault handler와 preempt handler가 서로 다른 `sigaltstack()` 소유권을 주장하지 않는다.
landing은 해당 M incarnation의 scheduler continuation으로 **돌아오지 않는 전환**을 수행한다.
M landing 위의 C 호출 frame을 중단된 G의 일반 `ctx`에 남기지 않는다.

첫 C landing 함수보다 먼저 ABI stack alignment와 scheduler 실행 환경을 설치한다.
원래 FP/control 상태는 그 전에 capsule에 보존해야 한다.
Scheduler는 도착 사유 `ASYNC_CAPTURED`를 처리한다. 원래 G의 register/errno를
landing의 값으로 덮어쓰는 ordinary yield wrapper를 재사용하지 않는다.
이 시점에 capsule 완성, G stack 이탈, owner-local borrow 종료를 확인한 뒤:

1. G의 `context_kind=ASYNC`와 suspend sequence를 확정한다.
2. 현재 G publication을 철회하되 scheduler-owned reference를 유지한다.
   P를 넘길 경우 old-ticket 신규 owner-side 작업도 닫은 뒤 quiescence를 commit한다.
3. 지속적인 cancellation 의도와 **wait/operation generation에 결부된 completion**을 따로 병합한다.
   결과와 참조는 기존 completion 객체가 소유하고, 해당 세대에서 한 번만 처리한다.
   무세대 `wake_pending` bit로 다음 wait까지 깨우지 않는다. 중간 상태의 waker는 독립 enqueue하지 않는다.
4. capsule generation/context kind/resume 제약을 완성하고 scheduler reference를 queue reference로
   공백 없이 이전한다. 선형화 지점은 단순 `state=READY` store가 아니라 **큐의 소비 가능 publication**이다.
5. 필요한 P 반환은 기존 lease transaction으로 처리한다. 큐 접근에는 여전히 유효 ticket이 필요하다.

일반 yield/park/direct-handoff에도 동일한 save-before-consume 규약을 적용한다.
Direct G→G fast path는 outgoing context 저장 뒤에만 이전 G가 소비되도록 증명된 경우에 유지한다.
증명 전에는 해당 async profile에서 scheduler-mediated 전환을 쓰며, 이것을 legacy 전체 disable로 숨기지 않는다.

`borrow == 0`이라는 순간값만으로 handback하지 않는다. QUIESCENT_ACK는 진행 중 조작의 종료뿐 아니라
옛 실행권으로 새 조작을 시작할 수 없음을 뜻한다. 새 ticket 이전에는 owner-side 처리를 재개하지 않는다.

### 6.4 재개와 restore transaction

Consumer는 origin-M **slot과 incarnation**, CPU capability 등의 적격성을 먼저 확인한 뒤
acquire 및 `READY_ASYNC → RESUMING` unique claim을 수행한다.
부적격 M이 claim부터 해놓고 origin M을 기다리면 안 된다.
FP 환경/stack/code refs를 확인하고 dispatch epoch를 새로 만든다.
캡처 당시 P ticket은 provenance일 뿐이고, 재개 권위는 현재 M이 새로 얻은 유효 ticket에서 나온다.
COOP와 ASYNC는 저장 형식이 다르므로 restore 경로도 구분한다.

Linux x86-64의 우선 조사 후보는 **native signal-return 계약에 맞춘 full-state restore**다.
기존 XRSTOR+GPR jump 경로에 process-global resume 주소만 M별로 바꾼 것을 완료로 간주하지 않는다.
후보 backend는 예약 신호가 차단된 상태에서 M-owned arena를 준비하고,
mask/PC/SP/register 재진입을 관측상 일관되게 완료해야 한다.
필요한 frame layout·내부 pointer·kernel 검증·CPU extension 처리는 R1 native qualification 대상이다.
단순 `setcontext()`가 full async restore라는 가정도 하지 않는다.

Restore attempt record R은 runtime instance/run, M incarnation, G generation, capture sequence,
새 dispatch sequence, restore sequence를 식별한다. 미리 공개한 epoch는 시도 식별자이지 완료 증거가 아니다.

Backend는 `reentry_witness(R, 실제 중단 context)`를 정의해야 한다.
참이면 해당 restore가 user-context 재진입을 통과했고 backend가 옛 restore 저장공간을 더 읽지 않는다는 뜻이다.
PC/SP가 인증된 G context에 속하고 landing/restore stub이 아닌지, binding/G/restore sequence가 맞는지,
host-signal/foreign callback이 같은 leaf를 호출한 상황이 아닌지 확인한다.
**PC 범위·stack 범위만으로 witness를 구성하지 않는다.** Backend control-flow와 callback scope 증명이 필요하다.

Witness 판정은 `RESTORING/RESUME_UNCONFIRMED`에 대한 일반 capture 거절보다 먼저 한다.
그렇지 않으면 성공 복원된 no-poll loop가 software ACK를 못 하고 영원히 거절될 수 있다.
실제 restore stub PC는 계속 거절한다. User poll뿐 아니라 검증된 handler와 scheduler entry도 witness가 된다.
이는 user context가 관측 가능해졌다는 뜻이지 user instruction 하나가 완료됐다는 뜻은 아니다.

Handler는 정확한 restore sequence의 `OBSERVED`만 bounded operation으로 공개한다.
실제 free/ref release/reuse는 일반 retirement 경로가 수행한다. 회수 조건은
`witness 성립 ∧ 해당 세대 모든 reader 종료 ∧ 다른 생존 의무 해소`다.
Watchdog/debug reader와 code/live-frame 참조도 빠뜨리지 않는다.

같은 handler가 C0의 restore witness를 얻고 새 capture까지 수행할 때는 **C1 bank**를 사용한다.
Scheduler landing 뒤 C0 reader가 끝난 것을 확인하고 C0/옛 restore arena를 retirement한다.
다음 restore 전에는 이전 미확인 attempt를 정리한다. `IDLE` store만으로 bank를 재사용하지 않는다.
충분한 storage가 없거나 reader가 남으면 overwrite하지 않고 명시적으로 defer한다.

늦은 옛 signal은 restore witness에 기여할 수 있지만 새 capture 권한은 아니다.
Capture commit에서 옛 request를 소비하고, 새 dispatch에 유효한 별도 request가 있을 때만 재포착한다.
즉시 복원·재포착 반복이 정상 user progress처럼 계수되는 것을 막는다.

Guaranteed-managed admission에는 예약 신호가 해당 실행에서 전달 가능한 mask 계약이 포함된다.
원래 mask를 몰래 바꾸거나 masked context를 받아들인 뒤 ACK를 기다리지 않는다.
Witness가 없으면 계속 pin하며 timeout은 회수 증거가 아니다. 준비 취소는 backend가 **user 미진입**을
증명한 경우에만 retry 가능하다. 실행 여부가 불명확한 restore 실패는 비복구 내부 오류이며
READY로 돌려 partial-effect를 재실행하지 않는다. Native thread의 최종 종료 증거는 transient 의무를
종료할 수 있지만 G의 정상 완료를 뜻하지 않는다.

### 6.5 Memory ordering과 lifetime 표

| 경계 | 요구 동기화 | 보호하는 것 |
| --- | --- | --- |
| dispatch descriptor publish/read | writer release / handler acquire, immutable payload | 완성된 tuple과 기존 pin |
| request publish/claim | REQUESTED release / 단일 CAS acquire-release | 요청 payload 변경 금지 기간 |
| capsule completion | release 후 scheduler acquire | register/extended payload의 완성 |
| READY publication/dequeue | release / acquire + unique state claim | save-complete와 실행권 이동 |
| wake while parking/capturing | state CAS/전이 lock + wait/operation 세대, 별도 persistent cancel | lost wakeup/옛 completion의 새 wait 오염 방지 |
| restore retirement | backend witness + exact restore sequence + 모든 reader 종료 | M arena/G capsule 재사용 시점; 관측 ACK와 free는 다름 |
| reclamation | 실행·queue·wait·signal·restore·foreign·code refs 모두 소진 | generation만으로 못 막는 use-after-free |

이는 C11/OS extension 구현의 의무다. 이 표만으로 weak-memory proof가 끝난 것은 아니다.
모든 실제 atomic은 writer/reader·linearization point·lifetime을 코드 옆에 설명해야 한다.

## 7. Foreign·TLS·lock·callback 계약

### 7.1 정상 foreign 진입

호출 전에 경계가 알려진 경우 P-local borrow와 runtime critical section을 끝내고,
기존 generation-checked detach로 P를 반환한다. Native 실행은 원래 M에서 계속하며,
다른 M은 ACK된 P만 실행한다. 복귀 시 이전 ticket을 부활시키지 않고 fresh ticket을 얻는다.
Depth·token·return claimant·STOPPING unwind는 하나의 transaction으로 일관되어야 한다.

새 managed-async 실행은 더 엄격한 admission을 요구한다. **Origin-M에 매인 suspended G가 있는데
그 M을 다른 G의 무제한 native 호출에 맡기면, 선점에는 성공해도 원래 G의 재개는 막힌다.**
따라서 origin-M async worker는 다음 범위만 받아들인다.

- 인증된 managed 연산과 실제로 G를 park시키는 LLAM wait.
- 호출 계약상 다른 OS thread에서 실행해도 되는 declared offload thunk.
- 미리 감사한 짧은 native 영역. 상한 없는 inline native 호출은 이 범주가 아니다.

동일 native thread가 필요한 장기 실행은 task/activation 입구부터 exclusive island로 보낸다.
`llam_enter_blocking()`의 legacy inline 동작을 guaranteed-managed profile에서 무조건 허용하지 않는다.
Origin-M continuation이 남으면 M retirement/offline을 금지하며, 제약된 ready G를 그 M의 큐에 남겨
fairness accounting과 wake를 수행한다. P가 잠시 없더라도 해당 M이 다시 P를 얻는 경로가 필요하다.

알려지지 않은 foreign 호출 안에 이미 들어갔다면 watchdog이 P를 강탈하지 않는다.
위반을 기록하고 독립 작업은 사용 가능한 별도 P/예비 자원에서 진행시킨다.
유한한 reserve가 모두 차면 backpressure를 적용하며 모든 작업의 무조건적 진행을 약속하지 않는다.

### 7.2 Native island와 용량

Native island는 단순한 home-P pin이 아니라 해당 activation의 **native thread exclusive 소유**다.
Raw TLS 주소, pthread identity, native lock ownership, GUI/COM 등 thread-affine 자원을 유지한다.
한 번 사용한 worker를 임의의 다른 identity-sensitive activation에 재사용해도 되는지는 해당 외부 계약에 달렸다.
영구 thread identity가 필요하면 dedicated lifetime을 유지한다.

Runtime별 native-active/queued 한도, process 내 LLAM runtime의 협조적 총량 한도,
관리 실행과 I/O/control을 위한 reserve를 둔다. 이는 비협조적인 다른 process나 third-party pool의
전체 CPU 사용량을 통제한다는 뜻이 아니다. Native 보유자가 다시 실행될 OS CPU 여유를 남기고,
admission 대기는 scheduler M을 blocking시키지 않는다.

예비 thread 하나가 재귀 callback/중첩 offload의 모든 deadlock을 해결한다고 가정하지 않는다.
의존성 cycle 및 capacity exhaustion은 거절/진단 대상으로 모델링한다. 한도 초과 시 새 실행을
거절하거나 G를 park하며, 부분 실행한 foreign 함수를 다른 executor에서 재시도하지 않는다.

### 7.3 잠금과 callback

Managed mutex는 G owner와 parkable wait를 사용한다. 보유 G의 비동기 선점은 가능하지만,
priority inversion을 제한하도록 lock-holder에 bounded boost/donation을 적용할 수 있어야 한다.
native pthread mutex/recursive mutex/spinlock/allocator lock은 동일한 것으로 취급하지 않는다.
Native lock scope가 끝나기 전에는 외형상 safe leaf를 호출해도 schedule-safe로 승격하지 않는다.

동기 foreign callback은 caller의 native/unsafe 범위를 상속한다. Callback이 managed 작업을
필요로 하면 검증된 trampoline과 fresh P ticket, 명시적인 재진입/대기 규칙을 거친다.
외부 caller가 native lock을 가진 채 기다리는 경우, 그 callback을 다른 G와 같은 M에
multiplex해도 되는지 별도로 증명한다. 불가능하면 callback은 island에 머문다.
외부 ABI가 허용하지 않는 동기→비동기 변환은 하지 않는다.

`longjmp`, 예외 unwind, `pthread_exit`, native asynchronous cancellation이 runtime transition을
건너뛰는 것은 초기에 지원하지 않는다. 지원하려면 compiler cleanup/token 복구/stack retirement
증거가 필요하다. 일반 C signal handler에서 LLAM API를 호출하는 것도 기본 계약이 아니다.

### 7.4 TLS와 실행 환경

일반 native TLS를 G TLS처럼 복제하거나 다른 M의 TLS base로 덮어쓰지 않는다.
G-local storage는 기존 LLAM task-local API 또는 전용 compiler lowering을 쓴다.
Migration 인증에는 현재 PC뿐 아니라 live caller frame의 TLS address·native identity 의존도 포함한다.
`errno` 값의 저장만으로 캐시된 `&errno`를 해결할 수는 없다.

중단 시점의 user FP control/status를 보관하고 landing/scheduler에는 별도의 scheduler FP 환경을 설치한다.
중간 다른 G의 rounding mode·exception flags가 영향을 주지 않게 한다.
Windows의 LastError/CRT errno, signal mask, TLS destructor, robust/rseq/thread-affine 상태는
각 backend의 명시된 지원 목록에 있어야 한다. 목록 밖은 추측 복원하지 않는다.

## 8. Compiler와 loader: 자동 선택을 가능하게 하는 부분

### 8.1 두 종류의 인증

PC interval 인증은 snapshot의 구조적 조건을 말한다. 실행 영역 인증은 caller의
native lock/TLS/foreign 상태와 인터리빙 허용 조건을 말한다. 런타임은 두 조건의 교집합만 사용한다.
Annotation 하나가 애플리케이션 전체의 논리적 원자성을 증명하지는 않는다.

새 metadata v2에는 적어도 다음 정보가 필요하다.

- 버전·OS/ISA/ABI·실제 build ID·code generation·확장 상태 요구.
- 최종 machine-code PC 범위와 resume kind, stack 안정성/필요 여유.
- async/schedule 허용, migration 허용, foreign/native-affine/no-preempt 영역.
- entry/call-closure 계약과 미인증 indirect-call 경계.
- compiler-generated poll/unsafe-exit 경계 및 coverage 검증 결과.

현재 v1 record와 실험용 수동 range는 유지할 수 있으나 guaranteed profile의 인증서로 승격하지 않는다.
IR pass 이후 backend lowering, stack probe, libcall, TLS resolver, sanitizer/profiler,
LTO/inlining/tailcall/ICF/link relocation이 바꾼 실제 code를 최종 검사한다.
Instruction restore 가능성과 unwind/debug metadata의 정확성도 구분한다.

### 8.2 진행 coverage

각 장시간 CFG cycle·irreducible loop·recursion·call chain이 다음 중 하나에 속해야 한다.

1. 유효한 인터럽트를 받을 때 async 중단 가능한 인증된 실행.
2. 유한한 경로 길이로 자동 poll/unsafe-exit 경계에 도달하는 실행.
3. 사전 foreign/native-exclusive 전환.

Unsafe PC에서 계속 거절된 요청은 pending으로 남고, unsafe-exit poll이 요청을 소비한다.
Retry 빈도만 높이는 것은 결정론적 coverage 증명이 아니다. 무제한 unsafe loop는 required-managed
admission을 통과시키지 않는다. Instruction 경로 길이는 page fault/OS 대기까지 포함한 wall-clock 상한이 아니다.

간접 호출은 target 계약을 검사한 후 외부 경계를 선택하며, effect가 시작된 뒤 분류하지 않는다.
Unknown asm, JIT, 동적 코드 patch는 별도 검증 전 native/compatible 경로다.
Compiler가 판단할 수 없는 application critical scope는 adapter/명시적 계약 또는 island가 필요하다.

### 8.3 자동 등록과 unload

최종 목표는 `llam-clang`/build integration이 metadata를 방출하고 runtime/module loader가 등록하여
실행 경로를 고르는 것이다. 사용자가 `llam_magic2_cpu_plan_*`나 private safe-range API를 호출할 필요가 없다.
첫 구현은 정적·수명 고정 모듈부터 시작한다. 런타임 종료까지 module image와 metadata를 pin한다.
이 pin은 실제 unload·주소 재매핑·code mutation을 막아야 한다. Metadata 사본이나 build ID만 보관하는 것은 pin이 아니다.

동적 unload는 후속 protocol이다: 새 진입 차단 → active G/caller-return-frame/capsule refs drain →
handler metadata reader grace period → table retire → OS image unload. 현재 PC 밖에 있는
return address도 code를 참조한다. 함수 주소만 보고 모듈이 사용 중인지 판단하지 않는다.
Handler에서 `dladdr`/`dlsym`/loader lock을 사용하지 않는다. ASLR 재사용에는 code generation을 대조한다.
Restore 후에는 capsule의 code 참조를 running G/live frame의 참조로 공백 없이 이전한다.
Capsule retirement가 caller return-address의 생존 의무를 종료시키지 않는다.

## 9. 플랫폼·CPU-state 승인 행렬

아래는 목표와 release gate이지 현재 지원 선언이 아니다. 일반 LLAM I/O backend 지원과
async context backend 지원을 분리한다.

| 대상 | interrupt/restore 후보 | 반드시 닫을 gate | 현재 판단 |
| --- | --- | --- | --- |
| Linux x86-64 | targeted signal, native-frame 기반 full restore 조사 | GPR/RFLAGS/x87/MXCSR/SSE/AVX 및 허용 XSTATE, red zone, mask, altstack, restore window | 실험 기반만 있음. native R1 필요 |
| Linux aarch64 | targeted signal + 별도 arm64 return backend | GPR/SP/PC/PSTATE, FPSIMD/FPCR/FPSR, SVE/SME/TPIDR2/VL, signal record | 신규 backend/네이티브 검증 필요 |
| macOS arm64 | Darwin signal/context-return backend | Apple ABI red zone, opaque thread state, PAC/arm64e, 지원 확장 상태와 host signals | 현재 호스트의 cooperative 통과는 근거가 아님 |
| macOS x86-64 | Darwin 전용 signal/context backend | Darwin frame/FP/AVX/stack/mask 규약 | Linux x86-64 코드를 그대로 승인하지 않음 |
| FreeBSD/OpenBSD/NetBSD x86-64 | 각 OS signal/context backend | OS-version별 frame·xstate·mask·thread lifetime | 각각 독립 qualification |
| DragonFly BSD x86-64 | 별도 연구 backend | 기존 experimental 플랫폼 gate 포함 | 초기 guaranteed release 제외 |
| Windows x86-64 | 검증된 own-M suspend/context-check/redirect/resume | stable HANDLE, balanced resume, controller 무잠금 구간, XSTATE/SEH/unwind/CFG/CET/error state | 별도 native 구현·검증 필요 |
| Windows arm64 | architecture 후보 | ARM64 context, unwind/보호 기능, runtime 자체 native 지원 | 현 LLAM platform 목록 밖; 이번 rollout 필수 범위로 확대하지 않음 |

Windows의 일반 APC는 alertable wait가 없는 busy loop의 선점 수단이 아니다.
Suspend 기반 후보는 external thread나 임의 process를 대상으로 하지 않으며, runtime-owned target만
짧게 멈추어 안전성을 확인한 후 즉시 재개한다. Controller가 멈춘 target의 lock을 기다려서는 안 된다.

지원하지 않는 CET/PKRU/PAC/SME 상태를 조용히 끄거나 일부만 저장하지 않는다.
feature probe는 compile-time macro, CPU capability, OS permission, 현재 M 설정을 모두 반영한다.
CPU 이동·feature/VL 변경은 재검증 경계다. 조건이 달라지면 새 admission을 닫고 안전한 경로로 drain한다.
이미 중단된 G를 복원할 capability가 사라졌다고 버리거나 무관한 M에서 실행하지 않는다.

## 10. Scheduling·I/O·Magic2 정책

정확성 protocol과 실행 정책을 분리한다. Magic2나 autotuner가 안전 조건을 변경할 수 없다.

새 스케줄러 정책은 현재 hot/norm/inject/overflow 큐를 재사용할 수 있지만,
fresh/latency-critical 작업에 주는 우선권에는 burst 한도와 aging/최소 CPU share가 필요하다.
계속 새 작업이 들어와 preempted G가 영원히 실행되지 않는 정책은 허용하지 않는다.
Affinity-constrained G도 대기 시간과 CPU share의 분모에 포함한다.

각 dispatch마다 runnable뿐 아니라 timer deadline, I/O completion, remote injection을 bounded batch로
확인한다. Direct handoff chain도 시간/횟수 한도에서 scheduler service로 돌아온다.
Timer가 미래에 하나 존재한다는 이유만으로 불필요한 고빈도 interrupt를 계속 보내지 않는다.
Due deadline·대기 작업·I/O 압력과 각 G의 실제 실행 구간을 기반으로 요청한다.

1 ms watchdog period와 1 ms quantum을 설정했다고 1 ms 이내 response가 성립하지 않는다.
Request cadence, 전달, unsafe deferral, scheduling과 OS off-CPU 지연을 분리해서 측정한다.
매우 짧은 quantum은 별도 고해상도 trigger 비용 평가 뒤에만 선택한다. 고정 dedicated busy-spin core를
portable 기본값으로 몰래 추가하지 않는다.

Magic2는 승인된 buffer transform의 plan/kernel/grain을 고르는 private 계층으로 남긴다.
각 plan의 실제 함수·호출 경로가 compiler/runtime 인증을 통과해야 async interrupt가 가능하다.
그렇지 않으면 기존 bounded tile yield 또는 적합한 blocking/offload 경로를 쓴다.
ZERO 초기화도 장시간 계산에 포함하며 runtime unsafe 구간으로 감추지 않는다.
중단은 같은 연산을 재개하는 것이지 새 plan으로 부분 결과를 다시 실행하는 것이 아니다.
Kernel 실패·LLAM 취소·이미 수행된 effect의 구분과 drain lifetime을 유지한다.

## 11. ABI·capability·오류 정책

기존 `OFF/COOPERATIVE/AUTO/STRICT`, `PINNED/NO_PREEMPT`의 값·기본 의미를 유지한다.
새 강한 계약을 `STRICT`라고 재명명하지 않는다. 내부적으로 다음 축을 별개로 표현한다.

- 요청 정책: 언제 선점이 필요한가.
- 메커니즘 capability: OS/ISA에서 어떤 capture/restore를 검증했는가.
- 코드 admission: 해당 task/module에 어떤 실행 계약을 적용할 수 있는가.
- 보장 수준: legacy, compiler-covered, async origin-M, async migratable, native-exclusive.
- 현재 상태: ready/active/degraded/disabled와 구체 사유.

초기 내부 opt-in 검증 후, public 노출이 필요하면 versioned 옵션의 tail에 추가하고
caller-size/compatible-prefix/default-zero 규약을 따른다. 이는 **향후 ABI 제안**이며 이번에는 header를 바꾸지 않는다.
Required 계약은 구버전 library가 모르는 tail을 무시하고 성공할 수 없도록 반환 capability handshake를
확인해야 한다. 필요하면 additive capability query를 도입하고 exports·SDK·ABI 테스트를 함께 갱신한다.
Magic2 public API를 새로 요구하지 않는다.

| 상황 | 계약 |
| --- | --- |
| optional backend/metadata 없음 | compatible 경로 + 사유 노출 |
| required 보장 제공 불가 | user effect 이전 admission 실패, `ENOTSUP` 의미의 명확한 진단 |
| capsule/native capacity 부족 | effect 이전 resource error 또는 park/backpressure; 무제한 thread 생성 금지 |
| stale/unsafe signal | 원래 실행 유지, pending/retry/diagnostic; 다른 G를 잘못 중단하지 않음 |
| unknown 필수 register state | capture 거절, capability 차단; 부분 snapshot 사용 금지 |
| restore 준비 실패 | backend가 user 미진입을 증명할 때만 attempt 취소/재시도 가능 |
| restore 실행 여부 불명확 | capsule/pin 보존, 비복구 내부 오류; cooperative downgrade나 effect 재실행 금지 |
| cancel/stop during capture | pending 의도 기록 후 상태 machine의 정상 commit/drain; 임의 stack 폐기 금지 |

## 12. 종료·peer runtime·signal 소유권

종료의 **새 작업/admission close**와 **기존 실행·retirement service close**를 분리한다.
먼저 새 작업과 신규 async admission을 닫고, 이미 승인된 G/foreign/I/O와
capture/landing/restore의 정상 drain·witness·필요한 재개/선점 서비스는 유지한다.
이 서비스로 해소할 reference를 기다리면서 먼저 handler를 닫으면 순환 대기가 생긴다.
필요한 실행/retirement 의무가 소진된 다음 새 sender 차단과 기존 sender 완료,
handler/binding quiescence → native join → signal/TLS/stack 자원 해제 순서로 넘어간다.
이미 캡처된 G에는 안전한 재개/정상 종료 경로가 있어야 한다. 무한 user loop의 선점이 가능하더라도
임의의 C cleanup을 생성할 수 없으므로 기존 cooperative cancellation의 완료까지 보장하는 것은 아니다.

Borrowed caller M과 owned native M을 구분한다. caller thread를 runtime이 terminate/join하지 않는다.
해당 M의 새 sender 참조를 차단하고 **이미 target handle/reference를 얻은 sender의 완료까지** 기다린 뒤 reserved signal을 block/drain하고,
자기 altstack/TLS/binding을 해제·복원하는 protocol이 필요하다.
마지막 runtime이라도 outstanding signal/binding/handler/retirement 의무가 남았으면 process signal action을 복원하지 않는다.
다른 host handler가 action을 교체했을 때 무조건 원래 값을 덮어쓰지 않는다.

Process broker는 runtime별 reference를 갖는다. A 종료로 B의 signal·metadata·worker가 무효화되면 안 된다.
동시 runtime stop, 같은 caller thread의 repeated run, 늦은 signal과 새 incarnation의 교차를 직접 검사한다.
Active worker가 있는 `fork` 이후 child 재사용은 별도 atfork/reinit 계약 전 지원하지 않는다.
Crash·hard timeout이 필요한 적대적 native 실행은 in-process preemption이 아닌 process isolation 문제다.

## 13. 구현 계획: 기존 Phase 이름과 구별한 R0–R6

각 단계는 implementation gate다. 이번 설계 작업에서 통과했다고 표시하지 않는다.
커밋 단위는 아래 불변식을 유지하는 작은 변경으로 나누고, Make/CMake/test 등록을 함께 한다.

| 단계 | 실제 변경 지점 | 종료 조건 |
| --- | --- | --- |
| R0 — 현재 수명 문제와 전환 규약 | `src/core/sched/scheduler.c`, `task/yield_join_sleep.c`, `wait/wait_tracking.c`, `base/errno.c`, `api/blocking_api.c`, shutdown 및 legacy tests | 기존 TSan 실패를 직접 재현·축소하고 원인 규명/수정. save-before-consume, foreign return, pause/stop quiescence를 ordinary/direct 경로에서 증명 |
| R1 — 제한된 native full-context 경로 | `context/async_context_linux_x86_64.c`, `src/internal/async_preempt_abi.h`, `src/asm/linux/x86_64/async_preempt_trampoline.S` | one-P 제한 유지하며 실제 hardware에서 full state/mask/stack/restore-window/witness oracle 통과. 준비 취소와 비복구 실패 구분. 후보 restore 방식 선택 근거를 남김 |
| R2 — capsule과 M mailbox 분리 | `src/internal/runtime_types.h`, task allocation/stack, async context, scheduler arrival | 여러 G가 동시에 suspended 가능. process-global resume PC/capture 슬롯 제거. 두 bank·reader retirement·stale signal·memory budget·unique claim 검증 |
| R3 — multi-M/P·foreign 공존 | lease/processor ticket, blocking API, watchdog, dynamic worker, lifecycle | origin-M 제약과 reserve/admission 처리, helper/redirect/callback/merge/shutdown 조합 통과. unknown M에서 P 강탈 없음 |
| R4 — compiler 및 자동 admission | `tools/preempt/llam_preempt_pass.cpp`, `metadata.{h,c}`, `tools/llam-clang`, build/package fixtures | v2 final-code coverage, compiler polls, call closure, static module 자동 등록. no-poll memory/FP workload의 실제 async 경로 확인 |
| R5 — peer·loader·platform 확대 | process signal broker, metadata lifetime, OS별 `src/core/context`/`src/asm`, CI | runtime A/B 동시 캡처·A 종료/B 생존. 각 OS/ISA/CPU-extension 개별 qualification. 동적 unload는 별도 하위 gate |
| R6 — 정책·성능·release | scheduler budgets, I/O service, debug capability, CPU-spike/bench, optional Magic2 consumer | correctness/ABI/packaging 통과한 exact revision에 matched regression. coverage/거절/자원 비용까지 승인한 뒤 opt-in→기본 정책 전환 여부 결정 |

R1과 R4의 compiler 계약은 함께 설계하지만, 넓은 코드 admission 활성화는 R0–R3 이후다.
R0의 기존 결함을 새 async 코드로 가리지 않는다. 제한을 전부 해제한 뒤 테스트를 추가하는 순서를 택하지 않는다.
Cross-M async migration은 R3/R4의 추가 gate이며, origin-M 모드와 독립적으로 승인한다.

## 14. 검증 행렬과 측정 규약

### 14.1 정확성 oracle

| 검증 축 | 반드시 포함할 경우 | 실패 판정 |
| --- | --- | --- |
| No-poll progress | 1P/다중 P를 CPU loop로 모두 점유, 다른 G·timer·socket·control injection | 외부 rescue로 loop를 풀어야만 진행하면 async 성공 아님 |
| State round-trip | 모든 선언된 GPR/flags/FP/SIMD/extended state, rounding/exception, errno/LastError, red zone/stack canary | 하나라도 의미가 달라짐, 미지원 상태를 성공 처리 |
| Multi-capsule | G1/G2를 번갈아 중단, M reuse, delayed/duplicate signal, ACK와 재포착 동시 발생, 이전 reader 유지, capacity exhaustion | snapshot overwrite, slot alias, 이중 resume, admission 후 storage 부족 |
| Transition races | capture×yield/park/wake/cancel/timeout/completion, 옛 wait completion×새 wait, direct handoff, suspend/resume boundary | context 저장 전 소비, lost wake, double enqueue/cleanup, 세대 오인 |
| Lease/foreign | primary/helper 둘 다 foreign, 반환 순서 역전, callback 재진입, same-M 요구 | old ticket 사용, 동시에 두 executor, origin-bound G의 M을 무제한 foreign에 넘김 |
| Lifecycle | stop/destroy×capture/restore/retirement, ownerless return, repeated run/borrowed M, 이미 handle을 얻은 sender, allocation/signal failure | 살아 있는 context/stack/code 해제, service를 먼저 닫은 drain 교착, join 누락, peer 영향 |
| Loader/compiler | LTO/최적화별 libcall/TLS/asm, unknown indirect call, stale metadata, DSO unload/주소 재사용 | 실제 code와 metadata 불일치, unsupported 경로 false-positive |
| Portability | native OS/ISA별 signal masks/ABI, 보호 기능 on/off, feature/VL 차이 | 다른 platform의 통과만으로 지원 표시 |

전환 테스트는 barrier/fault injection으로 순서를 고정한다. Sleep 타이밍만으로 경합을 유도하지 않는다.
Test hook은 test build에만 둔다. 긴 stress/soak는 direct regression을 보완하며 대체하지 않는다.
Sanitizer가 custom stack 전환을 이해하도록 integration을 확인하되, 보고된 race를 blanket suppression하지 않는다.
ThreadSanitizer 결과는 실제 sanitizer fiber annotations와 instrumentation 경로를 함께 기록한다.

기존 시작점은 `test_runtime_core`, `test_multi_runtime_core`, `test_runtime_shutdown_internal`,
`test_runtime_invariants`, `test_legacy_lease_internal`, foreign/relief/processor tests,
async/metadata/capability tests, sync/API/select/I/O 및 ABI/export tests다.
`test_*` Make target은 build이므로 binary 실행을 별도로 해야 한다.
변경이 완성되면 repository gate `make -j4 all test`, `make check-license-policy`,
CMake/CTest, 해당 ASan/TSan·platform gates를 final revision에서 수행한다.

### 14.2 성능 비교

기존 `scripts/cpu_spike_compare.py`와 `examples/cpu_spike.c`를 확장하여 다음 경로를 비교한다.

| 대조군 | 고정 조건과 목적 |
| --- | --- |
| 기존 LLAM cooperative | 같은 build/worker/workload, head-of-line blocking baseline |
| compiler-only | 같은 연산·정책, 자동 poll 비용과 coverage |
| certified async / hybrid | 같은 정책에서 interrupt 메커니즘의 추가 효과 |
| native exclusive / OS threads | native 호환성·메모리·oversubscription tradeoff |
| Magic2 off/on | 동일 kernel semantics와 buffer, 선점 경로와 policy 효과를 분리 |
| Go 등 외부 runtime | 보조 참고치; 언어·allocator·scheduler 차이를 선점 하나의 효과로 해석하지 않음 |

Request 생성, signal 도착, 거절/수락, scheduler commit, companion 첫 실행, 원래 G 재개 시각을 구분한다.
Timer lateness, socket completion→G 실행, 외부 control trigger→실행, throughput,
CPU 사용량/context-switch, RSS/stack/capsule 비용, native capacity/거절율을 함께 기록한다.
Preempted G의 최장 ready 대기와 lock-holder progress도 포함한다.

성공 capture만 뽑은 p99는 보장 수치가 아니다. 전체 시도/eligible dispatch 수와
unknown PC·unsafe span·foreign·busy slot·stale generation·signal 실패를 분모에 포함한다.
Native/legacy 영역은 별도 계층으로 집계하며, 전체 wall time의 몇 %를 managed guarantee가 덮는지도 보고한다.

Build flags/compiler/CPU features/worker 수/affinity/NUMA/주파수/열/호스트 부하/입력/seed를 고정한다.
Warmup 뒤 paired interleaved runs와 isolated process cases를 사용하고 raw sample을 모두 보존한다.
샘플이 적어 p99가 max인 경우를 명시하고, 반복 횟수보다 유효한 tail sample 수와 불확실성을 보고한다.
정책 튜닝에 사용한 workload는 개발 자료로 표시하고 최종 confirmation workload를 따로 둔다.

사용자가 정하지 않은 절대 μs SLO나 허용 overhead를 ‘이미 만족해야 하는 요구’로 발명하지 않는다.
R6 진입 전 대표 workload별 latency SLO·회귀 허용량·memory/native-thread budget을 고정한다.
초기 quantum sweep은 현 1 ms 기준 주변에서 시작할 수 있지만 합격선이 아니다.
기존 `scripts/bench_guard.py`의 프로젝트 기준을 완화해서 새 구현을 통과시키지 않는다.

### 14.3 Formal/model-check 의무

최소 추상 model에는 두 M, 두 G, 한 P, 중복/지연 signal, foreign return, wake/cancel/stop을 둔다.
Safety: single execution, save-before-ready, single P owner, no premature reclaim.
Liveness는 OS CPU 공급·event delivery·bounded unsafe path·자원 admission의 가정을 명시한 뒤 검사한다.
Fair scheduler를 가정하지 않은 모델에 무조건적인 starvation freedom을 주장하지 않는다.
현재 이 모델이나 weak-memory litmus는 **아직 실행하지 않았다**. 이는 구현 단계의 검증 계획이다.

## 15. 이번 연구에서 실제 확인한 것과 남은 것

| 항목 | 이번 실행 상태 |
| --- | --- |
| actual code/ABI/기존 실패 로그 대조 | PASS: source-level 관찰, 원인/전체 race-freedom 판정은 아님 |
| 최신 Pro 실제 협업 | PASS: 지정된 기존 세션, `6 Pro`에서 2회 완료 응답 전체 읽음. 수정 규약을 포함한 architecture를 RESEARCH-complete로 승인 |
| TLS compile-only 재현 | PASS: arm64 및 Linux x86-64 대상 IR에서 opaque call 전후 주소 재사용 확인 |
| 신규 runtime 구현·native async tests·성능 benchmark | NOT_RUN: 이번 design-only 범위 밖 |
| 기존 ownerless TSan 실패 해결 | OPEN: R0 필수 gate |
| native full restore/platform qualification | OPEN: R1/R5 필수 gate |
| 모든 임의 C 코드의 선점/이동/취소 | 주장하지 않음; domain 계약과 native island로 구분 |
| 커밋·push·merge·배포 | 수행하지 않음 |

AC1–AC5는 **설계 산출물 기준으로 충족**했다. Pro가 제시한 C′/D′, origin-M service,
retirement 규약을 반영했고 원문·코드·컴파일 실험의 범위를 대조했다.
adoption_status는 NOT_EVALUATED(구현 요청 아님), merge_readiness는 NOT_APPLICABLE이다.

최종 권장 순서는 R0의 현재 수명 문제를 먼저 닫고, R1의 native 복원 qualification,
R2의 다중 capsule, R3의 multi-M/P·foreign, R4의 compiler coverage,
R5의 platform/peer, R6의 실측 승인으로 확장하는 것이다.
독립 backend feasibility 연구는 R0와 병행할 수 있지만 통합 runtime 지원 승인은 R0를 건너뛰지 않는다.
완전한 런타임 지원이나 측정된 지연 보장은 설계 문서와 모델의 승인만으로 획득되지 않는다.
