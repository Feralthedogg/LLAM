<!--
Copyright 2026 Feralthedogg
SPDX-License-Identifier: Apache-2.0
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at https://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied. See the License for the
specific language governing permissions and limitations under the License.
See LICENSES/Apache-2.0.txt.
-->

# Native multi-M samples, final 14-case batch

GCC 15.2.0 MinGW x64 Release, fixed 2/3 primary Ms, unchanged R8 certified DLL.
Child process affinity selects concrete sparse CPU ids when available. No
manual captures/restores: the production controller owns automatic operations.
Migration tests force placement only after a valid ordinary READY publication.

Each mode: `test_windows_async_multim_oracle.exe MODE certified_fixture.dll`.
The executable self-bounds at 12 seconds; cases ran serially on the shared host.

## startup-stop

Exit: 0

```text
llam-win-multim: startup-stop passed workers=2 user_entries=0 expected=0
```

## repeat

Exit: 0

```text
llam-win-multim: old sweep held after debt=0; run token remains owned until controller transition release
llam-win-multim: M0 tid=32540 bank=2 chain=0/1/2/6 runs=1/1 -> 2/3
llam-win-multim: M0 tid=32540 bank=3 chain=8/9/10/13 runs=2/3 -> 3/5
llam-win-multim: M0 tid=32540 bank=4 chain=16/17/18/22 runs=3/5 -> 4/7
llam-win-multim: M0 tid=32540 bank=5 chain=24/25/26/29 runs=4/7 -> 5/9
llam-win-multim: M0 tid=32540 bank=6 chain=32/33/35/36 runs=5/9 -> 6/11
llam-win-multim: M1 tid=46564 bank=1 chain=3/4/5/7 runs=1/1 -> 2/3
llam-win-multim: M1 tid=46564 bank=2 chain=11/12/14/15 runs=2/3 -> 3/5
llam-win-multim: M1 tid=46564 bank=3 chain=19/20/21/23 runs=3/5 -> 4/7
llam-win-multim: M1 tid=46564 bank=4 chain=27/28/30/31 runs=4/7 -> 5/9
llam-win-multim: second-run generation=2 incarnation=2 old-generation refused before mutation
llam-win-multim: M0 tid=32540 bank=1 chain=38/39/41/42 runs=1/13 -> 2/15
llam-win-multim: M0 tid=32540 bank=2 chain=43/44/45/48 runs=2/15 -> 3/17
llam-win-multim: M0 tid=32540 bank=3 chain=50/51/52/55 runs=3/17 -> 4/19
llam-win-multim: M0 tid=32540 bank=4 chain=58/59/60/64 runs=4/19 -> 5/21
llam-win-multim: M1 tid=15236 bank=1 chain=40/46/47/49 runs=1/11 -> 2/13
llam-win-multim: M1 tid=15236 bank=2 chain=53/54/56/57 runs=2/13 -> 3/15
llam-win-multim: M1 tid=15236 bank=3 chain=61/62/63/65 runs=3/15 -> 4/17
llam-win-multim: M1 tid=15236 bank=4 chain=69/70/72/73 runs=4/17 -> 5/19
llam-win-multim: M1 tid=15236 bank=5 chain=74/75/76/77 runs=5/19 -> 6/21
llam-win-multim: repeat passed workers=2 user_entries=4 expected=3008183512622477815
```

## positive

Exit: 0

```text
llam-win-multim: M0 tid=42640 bank=1 chain=0/2/5/6 runs=1/1 -> 2/3
llam-win-multim: M0 tid=42640 bank=2 chain=9/11/12/15 runs=2/3 -> 3/5
llam-win-multim: M0 tid=42640 bank=3 chain=16/18/21/22 runs=3/5 -> 4/7
llam-win-multim: M0 tid=42640 bank=4 chain=26/27/28/31 runs=4/7 -> 5/9
llam-win-multim: M1 tid=65748 bank=1 chain=1/3/4/7 runs=1/1 -> 2/3
llam-win-multim: M1 tid=65748 bank=2 chain=8/10/13/14 runs=2/3 -> 3/5
llam-win-multim: M1 tid=65748 bank=3 chain=17/19/20/23 runs=3/5 -> 4/7
llam-win-multim: M1 tid=65748 bank=4 chain=24/25/29/30 runs=4/7 -> 5/9
llam-win-multim: positive passed workers=2 user_entries=2 expected=3008183512622477815
```

## overlap

Exit: 0

```text
llam-win-multim: overlapping_ops=2 pending_restore=1 max_suspend=1
llam-win-multim: M0 tid=10708 bank=1 chain=0/9/13/14 runs=1/1 -> 2/3
llam-win-multim: M0 tid=10708 bank=2 chain=18/19/20/23 runs=2/3 -> 3/5
llam-win-multim: M0 tid=10708 bank=3 chain=25/27/28/31 runs=3/5 -> 4/7
llam-win-multim: M0 tid=10708 bank=4 chain=33/34/35/36 runs=4/7 -> 5/9
llam-win-multim: M1 tid=66380 bank=1 chain=1/2/3/4 runs=1/1 -> 2/3
llam-win-multim: M1 tid=66380 bank=2 chain=5/6/7/8 runs=2/3 -> 3/5
llam-win-multim: M1 tid=66380 bank=3 chain=10/11/12/15 runs=3/5 -> 4/7
llam-win-multim: M1 tid=66380 bank=4 chain=16/17/21/22 runs=4/7 -> 5/9
llam-win-multim: M1 tid=66380 bank=5 chain=24/26/29/30 runs=5/9 -> 6/11
llam-win-multim: overlap passed workers=2 user_entries=2 expected=3008183512622477815
```

## migration

Exit: 0

```text
llam-win-multim: M0 tid=32784 bank=4 chain=12/13/14/15 runs=6/216550 -> 7/216552
llam-win-multim: M0 tid=32784 bank=5 chain=16/17/18/19 runs=7/216552 -> 8/216554
llam-win-multim: M0 tid=32784 bank=6 chain=20/21/22/23 runs=8/216554 -> 9/216556
llam-win-multim: M0 tid=32784 bank=7 chain=24/25/26/27 runs=9/216556 -> 10/216558
llam-win-multim: M1 tid=8636 bank=1 chain=0/1/2/3 runs=1/1 -> 2/3
llam-win-multim: M1 tid=8636 bank=2 chain=4/5/6/7 runs=2/3 -> 3/5
llam-win-multim: M1 tid=8636 bank=3 chain=8/9/10/11 runs=3/5 -> 4/7
llam-win-multim: free_remote_observed=1 owners_verified=0 stale_B_refused=1 errno/LastError preserved
llam-win-multim: migration passed workers=2 user_entries=1 expected=3008183512622477815
```

## owners3

Exit: 0

```text
llam-win-multim: allocation=0 immutable_debt=1 terminal_finalizer=2; execution=0 finalizer=1 all-three-Ms-live
llam-win-multim: M2 tid=49280 bank=6 chain=19/20/21/22 runs=9/253589 -> 10/253591
llam-win-multim: M2 tid=49280 bank=7 chain=23/24/25/26 runs=10/253591 -> 11/253593
llam-win-multim: M2 tid=49280 bank=8 chain=27/28/29/30 runs=11/253593 -> 12/253595
llam-win-multim: M2 tid=49280 bank=9 chain=31/32/33/34 runs=12/253595 -> 13/253597
llam-win-multim: M2 tid=49280 bank=10 chain=35/36/37/38 runs=13/253597 -> 14/253599
llam-win-multim: M2 tid=49280 bank=11 chain=39/40/41/42 runs=14/253599 -> 15/253601
llam-win-multim: M1 tid=65552 bank=1 chain=0/1/2/3 runs=1/1 -> 2/3
llam-win-multim: M1 tid=65552 bank=2 chain=4/5/6/7 runs=2/3 -> 3/5
llam-win-multim: M1 tid=65552 bank=3 chain=8/9/10/11 runs=3/5 -> 4/7
llam-win-multim: M1 tid=65552 bank=4 chain=12/13/14/15 runs=4/7 -> 5/9
llam-win-multim: free_remote_observed=1 owners_verified=1 stale_B_refused=1 errno/LastError preserved
llam-win-multim: owners3 passed workers=3 user_entries=1 expected=3008183512622477815
```

## delayed

Exit: 0

```text
llam-win-multim: delayed op_finish released fresh save; destination DEAD/capsule freed before old scan release; execution=0 finalizer=1 old_binding=1 both-Ms-live
llam-win-multim: delayed passed workers=2 user_entries=1 expected=3008183512622477815
```

## stop

Exit: 0

```text
llam-win-multim: overlapping_ops=2 pending_restore=1 max_suspend=1
llam-win-multim: M0 tid=27964 bank=2 chain=0/4/5/7 runs=1/1 -> 2/3
llam-win-multim: M1 tid=63384 bank=1 chain=1/2/3/6 runs=1/1 -> 2/3
llam-win-multim: stop passed workers=2 user_entries=2 expected=3008183512622477815
```

## destroy

Exit: 0

```text
llam-win-multim: overlapping_ops=2 pending_restore=0 max_suspend=1
llam-win-multim: public destroy ops=2 pending_restore=1 published before release
llam-win-multim: destroy passed workers=2 user_entries=2 expected=3008183512622477815
```

## alloc1

Exit: 0

```text
llam-win-multim: alloc1 passed workers=2 user_entries=0 expected=0
```

## create2

Exit: 0

```text
llam-win-multim: create2 passed workers=3 user_entries=0 expected=0
```

## dup0

Exit: 0

```text
llam-win-multim: dup0 passed workers=2 user_entries=0 expected=0
```

## dup2

Exit: 0

```text
llam-win-multim: dup2 passed workers=3 user_entries=0 expected=0
```

## probe2

Exit: 0

```text
llam-win-multim: probe2 passed workers=3 user_entries=0 expected=0
```
