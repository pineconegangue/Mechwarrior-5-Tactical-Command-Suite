# New command ideas

Source: a "MechWarrior player" agent who had not seen VoiceCommand's lists wrote 100 radio calls (user removed 3:
the two "Sledge" lines and "Don't shoot the containers"). Each was fed as text straight into HandleSpoken
(`VoiceCommand.exe --parsetest`, push-to-talk held, Atlas / Hunchback / Jenner / Catapult / Locust / Raven / Marauder /
Shadow Hawk on scan, no earlier order). Raw result: `out/build/poc-x64-Debug/ParseTest_report.tsv`;
input: `src/voice_command/PlayerCommands.tsv`. 2026-10-10.

**Score: 98 lines - 43 did what the player meant (incl. "copy that" / "belay that" correctly doing nothing), 9 near misses, 15 did something else, 31 not understood at all.**

## 1. Understood as meant (no work)
Lance, focus fire on my target / Two, hit that Atlas / Kill the Jenner, it's flanking us / Three, take out that
Catapult... / Somebody get that Raven off me / Hit the closest guy / Weapons free / Lance, form up on me / Regroup /
Follow me / Move to nav Alpha / Push forward / Advance, slowly / Two, move up a hundred meters / Three, go north, about
two hundred / Hold position / Hold the line here / Stay put, Three / Wait here till I call you / Stop, stop, everyone
stop / Fall back! / Fall back to me / Pull back / Four, get out of there, you're red / Retreat to the dropship / Back off
a bit / Hold fire / Cease fire / Fire at will / Switch to lasers / Alpha strike / Two, you're running hot, cool down /
Status report / Lance, sound off / Two, how you holding up? / Where are they? / Who's shooting at me? / Where are you,
Four? / Copy that (correctly nothing). Belay that / Never mind, as you were: nothing because the test gives no earlier
order to cancel - they work after an order.

Near misses (right idea, a detail lost): "Four, engage at will" -> attack my target (no free-target mode);
"Kill the turrets first" / "Take out the vehicles" -> attack my target (no target-class filter); "Line abreast on me" ->
follow; "Get up on that hill" -> move with no point; "Get out of the open" -> fall back; "Watch your heat" -> fire
settings (cool); "Three, attack my target. No wait, the one behind it, the Shadow Hawk" -> attack Shadow Hawk + a stray
move; "Two and Three, focus the Marauder; Four, keep the lights busy" -> Four's half dropped; "LRMs only, stay back" ->
LRMs only + a stray hold.

## 2. Misread - did something ELSE (fix first: these do harm)
| Said | Became | Should be |
|---|---|---|
| Spread out, you're bunched up for the arty | indirect fire | spread out (new) |
| Artillery! Move, move, move! | indirect fire | scatter / move out (new) |
| Incoming missiles, get to cover! | fire settings | take cover (new) |
| Watch your fire, Three, I'm right in front of you | move (lance) | friendly-fire warning: Three checks fire |
| Good kill, Two | attack my target -> Two | praise, a reply only |
| Don't shut down on me, guys | follow | run cool |
| Everybody vent and reset | reset fire orders | run cool / hold fire briefly |
| Stay close, don't wander off | hold | follow / tighten up |
| Hold here and don't push | move | hold |
| Lance, head to nav point Bravo and hold there | hold (here) | hold at Nav Bravo |
| Two, you're on my left. Three, my right | move Two | formation slots (new) |
| How's your ammo, Four? | "status of a unit named ammo" | Four's ammo report |
| Find some cover, you're getting shredded | where-question about "some cover..." | take cover (new) |
| Go to my target | say again | move to my target's position |
| Objective's the comm tower, hit the tower | say again | attack objective (new) |

## 3. Not understood at all - feature ideas
**Attack**
- "Everyone on the Hunchback, now!" - "on the X" with no verb as an attack.
- "Guys, go for the legs on the Atlas" - called shots (legs / arms / torso).
- "Light 'em up!", "Finish him off", "Go after the one that's shooting me" (target = the shooter on me; the traced-fire
  data already knows it).
- "Engage at will" as a real free-target mode; target classes: turrets / vehicles / lights first.
- "Try not to core him, I want that mech" - salvage-friendly fire (legs).

**Movement / formation**
- Spread out / tighten up / stay close - formation spacing.
- Line abreast, "Two on my left, Three on my right" - formation slots.
- Flank left / right (around a ridge) - flanking move.
- Take cover / get to cover / behind the buildings - cover seeking.
- Charge / close the distance - advance to the target at full speed.
- Scout ahead - one lancemate moves ahead.
- Move to where I'm looking / go to my target - move to the aim point.
- Next waypoint / next nav point.
- Disengage - fall back + hold fire.

**Defend**
- Defend the base, protect the convoy, guard the dropship, keep them off the generators - defend an objective.
- Cover me, protect me, watch my back - escort the player.

**Threats**
- Incoming missiles / artillery / air strike - scatter or take cover.
- "Reinforcements from the east" - face / engage a direction.

**Questions**
- Anyone low on ammo? Who's hurt bad? Three, what's your armour like? How's your ammo, Four? (per-topic status).
- How many hostiles left? Contacts? Anybody see that Atlas? What's the biggest thing out there? (several of these exist
  in other phrasings - "any contacts", "can you see the Atlas", "any assaults" - so most need only wording).

## 4. Cheapest wins (wording only, the feature exists)
"how many hostiles left", "contacts, where's the contacts", "anybody see that X", "what's the biggest thing" (any
assaults), "who's hurt bad" (anyone hurt), "anyone low on ammo" / "how's your ammo" (ammo question), "what's your armour
like" (how bad), "everyone on the X" (attack named), "go after the one shooting me" (who is firing -> attack), "head to
nav Bravo and hold there" (hold at nav), "hold here and don't push", "stay close" (follow), "disengage" (fall back),
"don't shut down" / "vent" (run cool), "good kill" (reply only, no order), "watch your fire" (no order).

## 5. Fixed 2026-10-10 (re-tested with --parsetest)
| Said | Was | Now |
|---|---|---|
| Four, engage at will | attack my target | weapons free + attack nearest (Four) |
| Two and Three, focus the Marauder; Four, keep the lights busy | Four's half lost | Two + Three attack the Marauder, Four attacks the nearest light |
| Spread out, you're bunched up for the arty | indirect fire | nothing (spread out not built) |
| Artillery! Move, move, move! | indirect fire | nothing |
| Incoming missiles, get to cover! | fire settings | nothing |
| Watch your fire, Three, I'm right in front of you | move | Three: "Copy, checking fire" |
| Good kill, Two | Two attacks your target | Two: "Thanks, boss" |
| Stay close, don't wander off / Tighten it up, guys | hold / nothing | follow |
| Hold here and don't push | move | hold |
| How's your ammo, Four? / Anyone low on ammo? | unit named "ammo" / nothing | ammo report |
| Two, you're on my left. Three, my right. | move Two | nothing (formation slots not built) |

## 6. Built 2026-10-14
- "Get up on that hill" / "take the high ground" -> the pilot marks 3-4 hilltops as nav points and asks which (HIGH GROUND).
- "Take out the turrets / tanks / VTOLs" -> a sweep through that class until none are left (TARGET CLASSES).
