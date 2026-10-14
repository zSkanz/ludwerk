# What a character carries reaches the other machines (protocol 44).
#
# A real server and a real joiner, two processes over the real transport. The
# server spawns a thing three ways -- from a stamp, as a `Clone` of a template
# in `ReplicatedStorage`, and with `Instance.new` -- each carrying a
# PointLight, a SpotLight, a cape's SpringBone and SpringCollider, a Bone, a
# Highlight, a Trail, a Beam, a ParticleEmitter with sequences of its own and a
# playing Sound. The joiner (`tests/netcode/carried/src/client/Seen.luau`)
# reads what it was sent of each against what the server holds, the sequences
# key by key; the server then writes one property of each, and the joiner
# reads them again. One line a class, a way and a phase:
#
#   [carried] spawned Stamped.PointLight same CFrame=... Brightness=3.0000 ...
#
# and this fails on any of them -- each class each way, the world's own, in
# both phases -- that is missing or says anything but `same`. It was every one of them but the emitter's before protocol 44 -- and
# the emitter's too, for the sequences it arrived without.
#
# Invoked as:
#   cmake -DHOST=<engine-host> -DPROJECT=<tests/netcode/carried> -DSTAGE=<dir>
#         -DPORT=<port> -P run_carried.cmake

foreach(required HOST PROJECT STAGE PORT)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "run_carried.cmake: -D${required}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}")
set(game "${STAGE}/game")
file(COPY "${PROJECT}/" DESTINATION "${game}" PATTERN ".engine" EXCLUDE)

# Both at once, as the acceptance gate runs its two: the joiner draws nothing
# (`--rhi=null`), so a runner with no GPU runs it. The frames are the most a
# slow machine is given; the joiner ends the run when it has said what it saw,
# and the server ends when the joiner leaves.
execute_process(
    COMMAND "${HOST}" --headless --pace=60 "--serve=${PORT}" --frames=3600 "${game}"
    COMMAND "${HOST}" --headless --rhi=null --pace=60 "--join=127.0.0.1:${PORT}" --frames=3000 "${game}"
    WORKING_DIRECTORY "${game}"
    RESULT_VARIABLE results
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)

# `Body` is the thing itself -- how it grips, what it weighs, whether a ray
# finds it -- and `Rig` a model under it with a primary part and a pivot: what
# an instance that always travelled was sent without, until this protocol.
set(classes PointLight SpotLight SpringBone SpringCollider Bone Highlight Trail Beam ParticleEmitter Sound Body Rig)
# And the world's own lines, which are no thing's.
set(world Lighting)
set(keys "")
foreach(way Stamped Cloned Made)
    foreach(class IN LISTS classes)
        list(APPEND keys "${way}.${class}")
    endforeach()
endforeach()
foreach(name IN LISTS world)
    list(APPEND keys "World.${name}")
endforeach()

set(failures "")
set(held 0)
foreach(phase spawned changed)
    foreach(key IN LISTS keys)
        string(REPLACE "." "\\." pattern "${key}")
        string(REGEX MATCH "\\[carried\\] ${phase} ${pattern} [^\n]*" line "${output}")
        if(line STREQUAL "")
            string(APPEND failures "  ${phase} ${key}: the joiner said nothing of it\n")
        elseif(NOT line MATCHES "^\\[carried\\] ${phase} ${pattern} same ")
            string(APPEND failures "  ${line}\n")
        else()
            math(EXPR held "${held} + 1")
        endif()
    endforeach()
endforeach()

# **And that what was read is what was authored**: `same` says the two ends
# agree, and two ends that both read a default would agree too. A few values a
# class, and every sequence key by key, as a time and its value.
function(expect phase class text)
    foreach(way Stamped Cloned Made)
        string(REGEX MATCH "\\[carried\\] ${phase} ${way}\\.${class} [^\n]*" line "${output}")
        string(FIND "${line}" "${text}" at)
        if(at EQUAL -1)
            set(failures "${failures}  ${phase} ${way}.${class}: not `${text}` in: ${line}\n" PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

set(fire "0.0000:1.0000,0.9000,0.5000/0.2500:1.0000,0.5000,0.1250/1.0000:0.2500,0.0000,0.0000")
set(fade "0.0000:0.0000~0.0000/0.7500:0.2500~0.1250/1.0000:1.0000~0.0000")
set(swell "0.0000:0.5000~0.0000/0.5000:2.0000~0.0000/1.0000:0.2500~0.0000")

expect(spawned PointLight "CFrame=0.0000,0.5000,0.0000 Color=1.0000,0.7500,0.5000 Brightness=3.0000 Range=12.0000")
expect(spawned PointLight "Shadows=true")
expect(changed PointLight "Brightness=7.0000")
expect(spawned SpotLight "Range=24.0000 Angle=30.0000 Enabled=false")
expect(changed SpotLight "Enabled=true")
expect(spawned SpringBone "RootJoint=Cape1 JointPattern=Cape* Stiffness=0.5000 Damping=0.2500 GravityScale=0.7500")
expect(changed SpringBone "Stiffness=0.8750")
expect(spawned SpringCollider "JointName=Spine Radius=0.3750 Length=0.5000 Offset=0.0000,0.1250,0.0000")
expect(changed SpringCollider "Radius=0.6250")
expect(spawned Bone "JointName=Head CFrame=0.0000,0.7500,0.0000 Transform=0.0000,0.2500,0.0000")
expect(changed Bone "Transform=0.0000,0.5000,0.1250")
expect(spawned Highlight "FillColor=0.2500,1.0000,0.2500 FillTransparency=0.7500")
expect(spawned Highlight "DepthMode=Occluded")
expect(changed Highlight "FillColor=1.0000,0.2500,0.2500")
expect(spawned Trail "Attachment0=A0 Attachment1=A1 Lifetime=1.5000 Texture=asset://textures/trail.png")
expect(spawned Trail "Color=${fire} Transparency=${fade} WidthScale=${swell}")
expect(changed Trail
       "WidthScale=0.0000:1.0000~0.0000/0.1250:3.0000~0.5000/0.5000:2.0000~0.0000/1.0000:0.0000~0.0000")
expect(spawned Beam "Attachment0=A0 Attachment1=A1 Width0=0.5000 Width1=0.2500 Segments=16 TextureMode=Static")
expect(spawned Beam "Color=${fire} Transparency=${fade}")
expect(changed Beam "Color=0.0000:0.0000,0.5000,1.0000/1.0000:1.0000,1.0000,1.0000")
expect(spawned ParticleEmitter "Rate=40.0000 Texture=asset://textures/flame.png Flipbook=4x2 FlipbookMode=OverLife")
expect(spawned ParticleEmitter "Collision=Terrain Bounce=0.2500")
expect(spawned ParticleEmitter "ColorOverLife=${fire} SizeOverLife=${swell} TransparencyOverLife=${fade}")
expect(changed ParticleEmitter
       "ColorOverLife=0.0000:0.5000,0.5000,1.0000/0.5000:0.2500,0.2500,1.0000/0.5000:1.0000,1.0000,1.0000/1.0000:0.0000,0.0000,0.5000")
expect(spawned Sound "Content=asset://sounds/torch.ogg Playing=true Looped=true Volume=0.2500 PlaybackSpeed=1.5000")
expect(changed Sound "Playing=false")
# The thing itself. **A ray dropped on it finds nothing while `CanQuery` is
# off, and the thing once the server turns it on** -- asked on the joiner, of
# the joiner's own physics.
expect(spawned Body "Friction=0.8750 Restitution=0.5000 Density=4.0000 LinearDamping=0.2500 AngularDamping=0.5000")
expect(spawned Body "Buoyant=false CanTouch=false CanQuery=false ContactDetails=true PivotOffset=0.0000,-1.0000,0.0000")
expect(spawned Body "Ray=miss")
expect(changed Body "Friction=0.1250")
expect(changed Body "CanQuery=true")
expect(spawned Rig "PrimaryPart=Handle PivotOffset=0.0000,0.5000,0.0000")
expect(changed Rig "PrimaryPart=Handle PivotOffset=0.0000,2.0000,0.0000")
foreach(way Stamped Cloned Made)
    string(REGEX MATCH "\\[carried\\] changed ${way}\\.Body [^\n]*" line "${output}")
    if(NOT line MATCHES "Ray=${way}[0-9]+")
        string(APPEND failures "  changed ${way}.Body: a ray does not find the thing in: ${line}\n")
    endif()
endforeach()
# The world's own.
foreach(pair "spawned|OutdoorAmbient=0.5000,0.2500,0.1250" "changed|OutdoorAmbient=0.1250,0.2500,0.5000")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 phase)
    list(GET pair 1 text)
    string(REGEX MATCH "\\[carried\\] ${phase} World\\.Lighting [^\n]*" line "${output}")
    string(FIND "${line}" "${text}" at)
    if(at EQUAL -1)
        string(APPEND failures "  ${phase} World.Lighting: not `${text}` in: ${line}\n")
    endif()
endforeach()

# The Highlight names its thing by reference: the joiner's copy must name the
# joiner's copy, whatever number its player was given.
foreach(way Stamped Cloned Made)
    string(REGEX MATCH "\\[carried\\] spawned ${way}\\.Highlight [^\n]*" line "${output}")
    if(NOT line MATCHES "Adornee=${way}[0-9]+ ")
        string(APPEND failures "  spawned ${way}.Highlight: its Adornee is not its thing in: ${line}\n")
    endif()
endforeach()

if(NOT output MATCHES "\\[carried\\] done")
    string(APPEND failures "  the joiner did not finish\n")
endif()

if(NOT failures STREQUAL "")
    message("${output}")
    message(FATAL_ERROR "carried: what did not reach the joiner as the server has it --\n${failures}")
endif()
list(LENGTH keys count)
math(EXPR expected "${count} * 2")
message("carried: ${held} of ${expected} lines the same on both ends, and what was authored")
