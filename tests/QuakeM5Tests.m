//
//  QuakeM5Tests.m — native XCTest wrapper around tests/smoke.sh
//
//  The real work lives in tests/smoke.sh (which also runs standalone from
//  the CLI and CI). This wrapper runs that script ONCE in +setUp, captures
//  its console output, and turns each "PASS:/FAIL:" line into a native
//  XCTest assertion so the results show up per-check in Xcode's Test
//  navigator when you press Cmd+U on the QuakeM5 scheme.
//
//  The repo root is derived from __FILE__ (this file lives in <repo>/tests),
//  so the script is found regardless of the test bundle's location.
//

#import <XCTest/XCTest.h>

static NSString *gOutput = nil;   // combined stdout+stderr of smoke.sh
static int gStatus = -1;          // its exit status (0 == all passed)

@interface QuakeM5Tests : XCTestCase
@end

@implementation QuakeM5Tests

+ (void)setUp
{
    NSString *root = [[@(__FILE__) stringByDeletingLastPathComponent] stringByDeletingLastPathComponent];
    NSTask *task = [[NSTask alloc] init];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/sh"];
    task.arguments = @[@"tests/smoke.sh"];
    task.currentDirectoryURL = [NSURL fileURLWithPath:root];

    NSPipe *pipe = [NSPipe pipe];
    task.standardOutput = pipe;
    task.standardError = pipe;

    // drain concurrently so a full pipe buffer can't deadlock the script
    NSMutableData *buf = [NSMutableData data];
    NSFileHandle *fh = pipe.fileHandleForReading;
    fh.readabilityHandler = ^(NSFileHandle *h) {
        [buf appendData:h.availableData];
    };

    NSError *err = nil;
    if (![task launchAndReturnError:&err]) {
        gOutput = [NSString stringWithFormat:@"could not launch tests/smoke.sh: %@", err];
        gStatus = -1;
        return;
    }
    [task waitUntilExit];
    fh.readabilityHandler = nil;
    [buf appendData:[fh readDataToEndOfFile]];

    gOutput = [[NSString alloc] initWithData:buf encoding:NSUTF8StringEncoding] ?: @"";
    gStatus = task.terminationStatus;
    NSLog(@"[smoke.sh]\n%@", gOutput);
}

// Assert that smoke.sh printed "PASS: <check>" and did not print "FAIL: <check>".
- (void)assertCheck:(NSString *)check
{
    XCTAssertNotNil(gOutput, @"smoke.sh produced no output");
    NSString *pass = [NSString stringWithFormat:@"PASS: %@", check];
    NSString *fail = [NSString stringWithFormat:@"FAIL: %@", check];
    XCTAssertTrue([gOutput containsString:pass],
                  @"expected PASS for \"%@\".\n--- smoke.sh output ---\n%@", check, gOutput);
    XCTAssertFalse([gOutput containsString:fail],
                   @"smoke.sh reported FAIL for \"%@\"", check);
}

- (void)testEngineBuilds        { [self assertCheck:@"engine builds (make sdl-release)"]; }
- (void)testQuakeCCompiles      { [self assertCheck:@"m5 QuakeC compiles"]; }
- (void)testPerfTierArmsMirror  { [self assertCheck:@"perf tier arms mirror the shipped table"]; }
- (void)testTierDetectRoundTrip { [self assertCheck:@"tier detection round-trips every tier"]; }
- (void)testGIBounceFallback    { [self assertCheck:@"rt: bounce light fallback armed under validation"]; }
- (void)testFroxelKernelsCompile { [self assertCheck:@"rt: froxel fog kernels compile under validation"]; }
- (void)testFroxelVolumeAllocated { [self assertCheck:@"rt: froxel fog volume allocated"]; }
- (void)testPipeliningKickArmed  { [self assertCheck:@"rt: pipelining kick armed"]; }
- (void)testASSkipStaticArmed    { [self assertCheck:@"rt: AS skip-static armed under validation"]; }
- (void)testSoundDumpOpens       { [self assertCheck:@"snd: the mix dump opens under -simsound"]; }
- (void)testSoundRoomReverb      { [self assertCheck:@"snd: the room reverb reads the level"]; }
- (void)testSoundDumpCloses      { [self assertCheck:@"snd: the mix dump closes with bytes in it"]; }
- (void)testSoundAirAbsorb       { [self assertCheck:@"snd: air absorption armed on a far sound (B3)"]; }
- (void)testStockWadArt          { [self assertCheck:@"stock: the world loads the original wad art"]; }
- (void)testStockNoReplacement   { [self assertCheck:@"stock: the replacement image is not loaded"]; }
- (void)testStockShellBox        { [self assertCheck:@"stock: the shell box has its own texture"]; }
- (void)testCheapOn              { [self assertCheck:@"cheap: the stock overlay goes on"]; }
- (void)testCheapOff             { [self assertCheck:@"cheap: and the player's look comes back"]; }
- (void)testCheapConfigRtMetal   { [self assertCheck:@"cheap: quitting with it on archives the player's own rt_metal"]; }
- (void)testCheapConfigViewscale { [self assertCheck:@"cheap: and the player's own render scale"]; }
- (void)testCheapNotArchived     { [self assertCheck:@"cheap: the overlay itself is never archived"]; }
- (void)testLiquidOwnLightArmed  { [self assertCheck:@"rt: liquid own light armed on e1m1's slime"]; }
- (void)testLiquidPairOn         { [self assertCheck:@"rt: liquid pair switched on (C2)"]; }
- (void)testLiquidPairBLAS       { [self assertCheck:@"rt: blended liquid acceleration structure built"]; }
- (void)testLiquidPairArmed      { [self assertCheck:@"rt: liquid pair armed at the slime (own term + reflection)"]; }
- (void)testWaterSurfaceArmed    { [self assertCheck:@"water: surface refraction armed on e1m1's slime"]; }
- (void)testSkyLightReadsADSun   { [self assertCheck:@"rt: the sky light reads AD's sun keys on start"]; }
- (void)testGIDilatedCull       { [self assertCheck:@"rt: dilated bounce cull armed under validation"]; }
- (void)testGamedirAutoMounts   { [self assertCheck:@"m5 gamedir auto-mounts"]; }
- (void)testHordeStarts         { [self assertCheck:@"horde director starts"]; }
- (void)testHordeWaveAnnounces  { [self assertCheck:@"horde wave 1 announces"]; }
- (void)testHordeSpawnPlacement { [self assertCheck:@"no bad horde spawn placements"]; }
- (void)testHordeNotarget       { [self assertCheck:@"horde notarget engages the brawl"]; }
- (void)testGorePreset2         { [self assertCheck:@"gore preset 2 applies quality 3"]; }
- (void)testGorePreset0Restores { [self assertCheck:@"gore preset 0 restores quality 1"]; }
- (void)testDoomShotgunFires    { [self assertCheck:@"Doom shotgun QuakeC branch fires"]; }
- (void)testExplosionSpriteOff  { [self assertCheck:@"explosion sprite: off branch taken"]; }
- (void)testShellCasingCell      { [self assertCheck:@"shell casing: procedural cell generated"]; }
- (void)testParticleCellsDustRing { [self assertCheck:@"particle cells: dust 35 and ring 36 generated"]; }
- (void)testParticleCellsFlashSpark { [self assertCheck:@"particle cells: flash 37 and spark 38 generated"]; }
- (void)testParticleCellsEmberDroplet { [self assertCheck:@"particle cells: ember 39 and droplet 40 generated"]; }
- (void)testParticleFont256Live { [self assertCheck:@"particle font: 256-pixel cells regenerate live"]; }
- (void)testSoftParticlesArmed { [self assertCheck:@"soft particles: armed under validation"]; }
- (void)testPartRefractArmed { [self assertCheck:@"particle refraction: armed under validation"]; }
- (void)testGIAmbientOcclusionArmed { [self assertCheck:@"rt: GI ambient occlusion armed under validation"]; }
- (void)testFogLiquidLightArmed { [self assertCheck:@"rt: fog light in the water armed under validation"]; }
- (void)testTorchEmbersLive { [self assertCheck:@"torch embers: emitter live"]; }
- (void)testSkyLightningFlash { [self assertCheck:@"rt: sky lightning flashed under validation"]; }
- (void)testCausticsArmed { [self assertCheck:@"caustics: armed under validation"]; }
- (void)testMuzzleFlashArmed     { [self assertCheck:@"muzzle flash: armed on the shotgun"]; }
- (void)testShellCasingNoFallback { [self assertCheck:@"shell casing: no silent fallback to the blob"]; }
- (void)testImpulsePentagram    { [self assertCheck:@"impulse 222 grants pentagram"]; }
- (void)testImpulseRingShadows  { [self assertCheck:@"impulse 224 grants ring of shadows"]; }
- (void)testBurnIgnites         { [self assertCheck:@"m5_burn: lightning ignites its target"]; }
// M5 enhanced thunderbolt. The node-count and gate checks are the regression net
// for the two defects that shipped during its development: a corrupt path array
// (which reported 49 nodes for 4 subdivision levels and drew uninitialised
// stack), and the master gate not actually gating.
- (void)testBoltNodeCount       { [self assertCheck:@"M5 bolt: builds with a sane node count"]; }
- (void)testBoltConnectSignal   { [self assertCheck:@"M5 bolt: knows when it is connecting"]; }
- (void)testBoltFlickerFloor    { [self assertCheck:@"M5 bolt: flicker floor holds"]; }
// The bolt's shape RNG must differ between re-rolls. It did not for the whole of
// this feature's first life - the seed's low word was constant and it is the only
// word the generator's output depends on - and nothing on screen said so.
- (void)testBoltSeedDecorrelated { [self assertCheck:@"M5 bolt: seed is decorrelated"]; }
- (void)testBoltNoBeamOverflow  { [self assertCheck:@"M5 bolt: no beam list overflow"]; }
- (void)testBoltMasterGate      { [self assertCheck:@"M5 bolt: master gate disables the whole path"]; }
- (void)testPhotoModeEnters     { [self assertCheck:@"photo mode enters"]; }
- (void)testPhotoModeExits      { [self assertCheck:@"photo mode exits"]; }

// Run G: menu integrity. A page whose drawn-row count disagrees with its
// *_ITEMS define gives the cursor phantom slots below the last row.
- (void)testMenuRowCounts       { [self assertCheck:@"menu: every page draws the rows it declares"]; }
- (void)testMenuCommands        { [self assertCheck:@"menu: every page command is registered"]; }
- (void)testIdleSwayEnvelope    { [self assertCheck:@"sway: envelope holds"]; }
- (void)testMapLightStat        { [self assertCheck:@"maplights: statistic prints"]; }
- (void)testMapLightNoRescaleId1{ [self assertCheck:@"maplights: nothing rescaled on id1"]; }
- (void)testVenomLight          { [self assertCheck:@"M5 venom: spit carries an acid light"]; }
- (void)testVenomMasterGate     { [self assertCheck:@"M5 venom: master gate disables it"]; }
// Ball lightning, the ninth weapon (run H2, BALLLIGHTNING.md slice 1, 2026-09-06).
- (void)testBallLaunches          { [self assertCheck:@"M5 ball: a ball launches"]; }
- (void)testBallArcs              { [self assertCheck:@"M5 ball: a ball arcs at a target"]; }
- (void)testBallEffectsResolve    { [self assertCheck:@"M5 ball: glow and burst effects resolve"]; }
- (void)testBallKnotDraws         { [self assertCheck:@"M5 ball: the plasma knot draws"]; }
- (void)testBallMasterGateRefuses { [self assertCheck:@"M5 ball: master gate refuses impulse 202"]; }
- (void)testBallMasterGateSilent  { [self assertCheck:@"M5 ball: master gate spawns nothing"]; }
// Weapon feel (run G, SEPTEMBER2 Part G, 2026-09-10): kick, grenade bounces,
// nail tracers, axe sparks -- each a first-event line, plus the all-off control.
- (void)testFeelKick              { [self assertCheck:@"M5 feel: weapon kick fires"]; }
- (void)testFeelNailTracer        { [self assertCheck:@"M5 feel: nail tracer set"]; }
- (void)testFeelNailBarrels       { [self assertCheck:@"M5 feel: nails from the barrels"]; }
- (void)testFeelGrenadeBounce     { [self assertCheck:@"M5 feel: grenade bounce sounds"]; }
- (void)testFeelAxeSparks         { [self assertCheck:@"M5 feel: axe sparks on a wall"]; }
- (void)testFeelAllOffSilent      { [self assertCheck:@"M5 feel: every cvar at 0 is silent"]; }
// The horde director (SEPTEMBER2 H, 2026-09-10): on in run A, silent at 0 in run H.
- (void)testHordeDirectorShapes    { [self assertCheck:@"horde director shapes wave 1"]; }
- (void)testHordeDirectorSilent    { [self assertCheck:@"horde director: silent at 0"]; }

// Run I: the mission packs. Skipped by smoke.sh when the gamedir is absent,
// since the pack data is the player's own Steam install and not in the repo.
- (void)testPackHipnoticMounts  { [self assertCheckOrSkipped:@"pack hipnotic: mounts as Hipnotic over m5"]; }
- (void)testPackHipnoticLoads   { [self assertCheckOrSkipped:@"pack hipnotic: hip1m1 loads its own progs"]; }
- (void)testPackRogueMounts     { [self assertCheckOrSkipped:@"pack rogue: mounts as Rogue over m5"]; }
- (void)testPackRogueLoads      { [self assertCheckOrSkipped:@"pack rogue: r1m1 loads its own progs"]; }
- (void)testPackDopaMounts      { [self assertCheckOrSkipped:@"pack dopa: mounts as Dimension of the Past over m5"]; }
- (void)testPackMg1Mounts       { [self assertCheckOrSkipped:@"pack mg1: mounts as Dimension of the Machine over m5"]; }
- (void)testPackHipnoticLook    { [self assertCheckOrSkipped:@"pack hipnotic: look file execs"]; }
- (void)testPackMg1Look         { [self assertCheckOrSkipped:@"pack mg1: look file execs"]; }

// The RT checks are skipped by smoke.sh on a machine with no Metal ray-tracing
// device, so treat "neither PASS nor FAIL printed" as a skip rather than a failure.
- (void)assertCheckOrSkipped:(NSString *)check
{
    XCTAssertNotNil(gOutput, @"smoke.sh produced no output");
    NSString *fail = [NSString stringWithFormat:@"FAIL: %@", check];
    XCTAssertFalse([gOutput containsString:fail],
                   @"smoke.sh reported FAIL for \"%@\".\n--- smoke.sh output ---\n%@", check, gOutput);
}

- (void)testDocsMatchTree
{
    [self assertCheck:@"docs match the tree (docsync)"];
}

- (void)testInertLiquidsReported
{
    [self assertCheck:@"rt: an inert rt_metal_liquids says so"];
}

- (void)testInertReportChangeOnly
{
    [self assertCheck:@"rt: the inert report is change-only"];
}

- (void)testRTCompositeRuns     { [self assertCheckOrSkipped:@"RT composite runs"]; }
- (void)testRTViewModelMask     { [self assertCheckOrSkipped:@"RT view-model mask active despite gamma"]; }
- (void)testRTMaskNotDisabled   { [self assertCheckOrSkipped:@"RT mask not silently disabled"]; }

// FOGLIGHT (run Q, 2026-08-28): both lightsample modes dispatched under API
// validation. Skipped without a Metal device like the rest of the RT family.
- (void)testFogLightsampleFogOnly { [self assertCheckOrSkipped:@"rt: lightsample fog-only mode engaged"]; }
- (void)testParticleLightingMode1 { [self assertCheckOrSkipped:@"particles: lighting mode 1 (engine LightPoint) lit a particle"]; }
- (void)testParticleLightingMode2 { [self assertCheckOrSkipped:@"particles: lighting mode 2 (irradiance grid) lit a particle"]; }
- (void)testDustEmitterLive       { [self assertCheckOrSkipped:@"particles: the dust emitter spawned motes"]; }
- (void)testParticleStaticScale   { [self assertCheckOrSkipped:@"particles: the static-light scale is live"]; }
- (void)testShadowLightsArmed     { [self assertCheckOrSkipped:@"RT: the extra lights are shadow-tested"]; }
- (void)testShadowLightsOff       { [self assertCheckOrSkipped:@"RT: the extra lights can be switched off"]; }
- (void)testBallClearsDoorway     { [self assertCheckOrSkipped:@"M5 ball: it clears the doorway"]; }
- (void)testPowerupGlow           { [self assertCheckOrSkipped:@"M5 powerups: the quad glows rather than spotlights"]; }
- (void)testFogLightsampleFull    { [self assertCheckOrSkipped:@"rt: lightsample full mode engaged"]; }
- (void)testTermUpsampleArmed     { [self assertCheckOrSkipped:@"rt: term upsample armed under validation"]; }
- (void)testFogDumpWritten        { [self assertCheckOrSkipped:@"RT fog dump: rt_snapshot writes the fog buffer beside the frame"]; }
- (void)testFogDumpParses         { [self assertCheckOrSkipped:@"RT fog dump: flicker.py parses the dump (metric live)"]; }
- (void)testFogClampMode1         { [self assertCheckOrSkipped:@"rt: fog clamp mode 1 (min/max) engaged"]; }
- (void)testFogClampMode2Tonemap  { [self assertCheckOrSkipped:@"rt: fog clamp mode 2 + tonemapped EMA engaged"]; }
- (void)testFogClampMode3         { [self assertCheckOrSkipped:@"rt: fog clamp mode 3 (decoupled EMA) engaged"]; }
- (void)testFogTemporalCompiles   { [self assertCheckOrSkipped:@"rt: the fog temporal kernel compiled"]; }
- (void)testFogReprojectDepthOn   { [self assertCheckOrSkipped:@"rt: fog history reprojection went translation-aware"]; }
- (void)testFogHybridMode1        { [self assertCheckOrSkipped:@"rt: fog light pick hybrid mode 1 (two rays) engaged"]; }
- (void)testFogHybridMode2        { [self assertCheckOrSkipped:@"rt: fog light pick hybrid mode 2 (alternating) engaged"]; }
// SMAA.md (2026-09-18): analytic MLAA at native resolution, three passes.
- (void)testSmaaEdgePassCompiles  { [self assertCheckOrSkipped:@"smaa: the edge pass compiles"]; }
- (void)testSmaaWeightPassCompiles { [self assertCheckOrSkipped:@"smaa: the weight pass compiles"]; }
- (void)testSmaaBlendPassCompiles { [self assertCheckOrSkipped:@"smaa: the blend pass compiles with 2 samplers"]; }
- (void)testSmaaArms              { [self assertCheckOrSkipped:@"smaa: the pass arms under validation"]; }
- (void)testSmaaSearchReaches     { [self assertCheckOrSkipped:@"smaa: the search reach reaches the pass"]; }
- (void)testSmaaDebugView         { [self assertCheckOrSkipped:@"smaa: the debug view arms"]; }
- (void)testSmaaSupersedesFxaaPost { [self assertCheckOrSkipped:@"smaa: it supersedes the FXAA post pass"]; }
- (void)testSmaaNoTargetRefused   { [self assertCheckOrSkipped:@"smaa: no intermediate target was refused"]; }
- (void)testSmaaValidationOn      { [self assertCheckOrSkipped:@"smaa: validation is really on"]; }
- (void)testSmaaNoMslFailure      { [self assertCheckOrSkipped:@"smaa: no MSL arm failed to compile"]; }
- (void)testFogHybridMode3        { [self assertCheckOrSkipped:@"rt: fog light pick hybrid mode 3 (single-pass) engaged"]; }
- (void)testRefitEngages          { [self assertCheckOrSkipped:@"rt: dynamic BLAS refit engages under validation"]; }
- (void)testAdaptiveStrideEngages { [self assertCheckOrSkipped:@"rt: adaptive fog stride engages under validation"]; }
// LIQUIDFOG (run Q, 2026-08-30): the murk's per-fragment liquid fade -- the one
// bed where USERTLIQUIDS and USEVOLUMETRICLIQUIDFADE are compiled at once, which
// is what proves DP_TEX_RTTERM's re-basing keeps the two off the same MSL index.
- (void)testLiquidFadeArmed       { [self assertCheckOrSkipped:@"murk: the liquid fade is armed under validation"]; }
- (void)testLiquidFadeWarns       { [self assertCheckOrSkipped:@"murk: blended liquid with no fade says so"]; }
- (void)testLiquidFadeChangeOnly  { [self assertCheckOrSkipped:@"murk: the liquid-fade report is change-only"]; }
// GIARC G1 (run Q, 2026-08-29): the one-bounce GI armed with walllight live.
- (void)testGIArmed               { [self assertCheckOrSkipped:@"rt: one-bounce GI armed under validation"]; }
// GIARC G3 (run Q, 2026-08-29): the emissive bounce's mode suffix on the same line.
- (void)testGIEmissiveArmed       { [self assertCheckOrSkipped:@"rt: emissive bounce armed under validation"]; }
// GIARC G4-1 (run Q, 2026-08-29): the GI sample-rate rotation under validation.
- (void)testGIRateArmed           { [self assertCheckOrSkipped:@"rt: GI rate rotation armed under validation"]; }
// GIARC G4-2 (run Q, 2026-08-29): the coloured bounce and its buffer-9 bind under validation.
- (void)testGIAlbedoArmed         { [self assertCheckOrSkipped:@"rt: coloured bounce armed under validation"]; }
// WATERSURFACE (run Q, 2026-09-12): the Metal uniform table dropped the murk's
// textures past its 128-name cap in silence and Seb's Cmd+R crash-looped; the
// table refuses out loud now, and run Q boots with r_watersurface 1 so the
// murk's constant struct is at its fattest under validation.
- (void)testUniformTableNotFull   { [self assertCheckOrSkipped:@"metal: uniform table is not full"]; }
- (void)testRtlightPassUnderValidation { [self assertCheckOrSkipped:@"metal: the rtlight pass draws under validation"]; }
- (void)testNoDuplicateReflection { [self assertCheckOrSkipped:@"metal: no name reflected at two locations"]; }

// The teleporter starfield (2026-09-01). The console line is the feature's only
// textual evidence -- its arm lives inside a static parm, which appears in no
// permutation number and no shader name -- and the still pair is the pixels.
- (void)testTeleportSwirlPivot    { [self assertCheckOrSkipped:@"teleport swirl: the shipped pivot reaches the shader"]; }
- (void)testTeleportSwirlChurn    { [self assertCheckOrSkipped:@"teleport swirl: churn rate moves the starfield (stills differ)"]; }
- (void)testTorchConeSoftness     { [self assertCheckOrSkipped:@"torch: cone softness reaches the kernel (stills differ)"]; }
- (void)testBoltFizzArmed         { [self assertCheckOrSkipped:@"m5bolt: the fizz reaches the shader"]; }
- (void)testEffectinfoParses      { [self assertCheckOrSkipped:@"effectinfo: m5/effectinfo.txt parses clean"]; }

// Run D: the full RT + in-kernel fog stack (also skipped without a Metal device).
- (void)testFogNoiseUploaded    { [self assertCheckOrSkipped:@"fog kernel: noise volume uploaded"]; }
- (void)testFogFieldUploaded    { [self assertCheckOrSkipped:@"fog kernel: world field uploaded"]; }
- (void)testFogBufferCreated    { [self assertCheckOrSkipped:@"fog kernel: output buffer created"]; }
- (void)testFogCompositeMaskUp  { [self assertCheckOrSkipped:@"fog kernel: RT composite mask up"]; }
- (void)testFogKernelCompiles   { [self assertCheckOrSkipped:@"fog kernel: no kernel compile fail"]; }
- (void)testFogKernelActive     { [self assertCheckOrSkipped:@"fog kernel: reports itself ACTIVE"]; }
// BEAUTY A1 (2026-09-16): the modern bloom chain -- its GLSL arm on run D's GL
// bridge, its MSL arm and its change-only armed line under run Q's validation.
- (void)testBloomM5GLCompiles   { [self assertCheckOrSkipped:@"bloom: the M5 chain's GLSL arm compiles on GL"]; }
- (void)testBloomM5MSLCompiles  { [self assertCheckOrSkipped:@"metal: the M5 bloom chain's shader compiles"]; }
- (void)testBloomM5ChainArms    { [self assertCheckOrSkipped:@"bloom: the M5 chain arms (levels and size)"]; }
// 2026-09-18: FXAA at native resolution after the MetalFX upscale. Its failure
// mode is silent in the picture, so the armed line is the only evidence.
- (void)testFXAAPostArms        { [self assertCheckOrSkipped:@"metal: FXAA runs at native resolution after the upscale"]; }

// Backstop: the script's own overall verdict.
- (void)testSuiteExitStatus
{
    XCTAssertEqual(gStatus, 0, @"tests/smoke.sh exited %d (a check failed)", gStatus);
}

@end
