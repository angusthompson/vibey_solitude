#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <vector>
#include <string>
#include <utility>
#include <fstream>

struct House {
    Vector3 position;
    Vector3 size;
    Color color;
    int side;                    // -1 or +1: which row it's in, i.e. which way its street-facing wall points
    std::vector<bool> windowsLit; // 2 entries (left/right) per floor row, bottom to top
    Vector3 doorPos;              // world position of the door, on the street-facing wall
    int paintingIndex;            // which loaded poster texture hangs inside this house's interior
};

struct Tree {
    Vector3 position;
    float trunkHeight;
    float foliageRadius;
};

// A simple wire fence: a line of posts and horizontal wires between two points, always
// running along z at a fixed x here (it only ever fills a gap between two houses in a row).
struct Fence {
    Vector3 start;
    Vector3 end;
};

struct Bin {
    Vector3 position;
};

enum PedestrianState { PED_WALKING, PED_ENTERING, PED_INSIDE, PED_EXITING };

struct Pedestrian {
    Vector3 position;
    int side;               // -1 or +1: which pavement they walk on
    float direction;        // +1 or -1: current walking direction along z
    PedestrianState state;
    float stateTimer;
    float doorCooldown;     // avoids re-rolling every frame while lingering near the same door
    Vector3 doorPos;        // just outside the door currently being approached/left
    Vector3 moveStartPos;   // position at the start of the current enter/exit lerp
    std::string line;       // this pedestrian's monologue line, shown when the player looks at them
};

// A stationary, black-hooded figure placed out past the edge of town - unlike Pedestrians,
// never moves and never enters a house.
struct ForgottenSoul {
    Vector3 position;
    std::string line;
};

// Splits a "Speaker: quote text" line into its two parts, trimming any leading space off the
// quote. Falls back to a generic speaker label if there's no colon to split on.
static void ParseDialogueLine(const std::string& line, std::string& speaker, std::string& quote)
{
    size_t colonPos = line.find(':');
    if (colonPos != std::string::npos)
    {
        speaker = line.substr(0, colonPos);
        quote = line.substr(colonPos + 1);
        while (!quote.empty() && quote.front() == ' ') quote.erase(0, 1);
    }
    else
    {
        speaker = "???";
        quote = line;
    }
}

// Greedy word-wrap: splits text into lines no wider than maxWidth at the given font size.
static std::vector<std::string> WrapText(const std::string& text, int fontSize, int maxWidth)
{
    std::vector<std::string> lines;
    std::string currentLine;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t spacePos = text.find(' ', pos);
        size_t wordEnd = (spacePos == std::string::npos) ? text.size() : spacePos;
        std::string word = text.substr(pos, wordEnd - pos);
        std::string testLine = currentLine.empty() ? word : currentLine + " " + word;
        if (MeasureText(testLine.c_str(), fontSize) > maxWidth && !currentLine.empty())
        {
            lines.push_back(currentLine);
            currentLine = word;
        }
        else
        {
            currentLine = testLine;
        }
        pos = (spacePos == std::string::npos) ? text.size() : spacePos + 1;
    }
    if (!currentLine.empty()) lines.push_back(currentLine);
    return lines;
}

struct CloudPuff {
    Vector3 position;
    float size;
    Color color;
};

struct StreetLight {
    Vector3 poleBase;
    Vector3 poleTop;
    Vector3 bulbPosition;
};

struct RainSplash {
    Vector3 position;
    float age;
};

struct Puddle {
    std::vector<Vector3> points; // triangle-fan outline around the puddle's own center
    Vector3 center;
    Vector3 nearestLightPos; // for the blurry warm glow reflected from the closest streetlight
};

// A small speckled texture: mostly `base`, with three speckle colors mixed in for grain.
static Texture2D MakeSpeckleTexture(int size, Color base, Color speckleA, Color speckleB, Color speckleC)
{
    Image image = GenImageColor(size, size, base);
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            int roll = GetRandomValue(0, 99);
            Color speckle = base;
            if (roll < 70) speckle = base;
            else if (roll < 85) speckle = speckleA;
            else if (roll < 95) speckle = speckleB;
            else speckle = speckleC;
            ImageDrawPixel(&image, x, y, speckle);
        }
    }
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    return texture;
}

// A single paving-slab tile: solid slab color with a grout-colored border on two edges, meant
// to be tiled at exactly one slab's world size so the repeats line up into a continuous grid.
static Texture2D MakeSlabTexture(int size, Color slabColor, Color groutColor, int borderPx)
{
    Image image = GenImageColor(size, size, slabColor);
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            if (x < borderPx || y < borderPx) ImageDrawPixel(&image, x, y, groutColor);
        }
    }
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    return texture;
}

// A dark metal drainage-grate texture: evenly spaced lighter bars over a near-black base.
static Texture2D MakeGrateTexture(int size)
{
    Image image = GenImageColor(size, size, Color{ 20, 20, 22, 255 });
    int bars = 5;
    int spacing = size / bars;
    for (int y = 0; y < size; y++)
    {
        for (int x = 0; x < size; x++)
        {
            bool isBar = (x % spacing) < 3;
            Color c = isBar ? Color{ 58, 58, 62, 255 } : Color{ 14, 14, 16, 255 };
            ImageDrawPixel(&image, x, y, c);
        }
    }
    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    return texture;
}

// A flat ground plane with its texture tiled at a fixed world-space size, so texel density
// stays consistent regardless of the plane's own width/length.
static Model MakeTiledGroundModel(float width, float length, float tileWorldSize, Texture2D texture)
{
    Mesh mesh = GenMeshPlane(width, length, 1, 1);
    float tilingX = width / tileWorldSize;
    float tilingZ = length / tileWorldSize;
    for (int i = 0; i < mesh.vertexCount; i++)
    {
        mesh.texcoords[i * 2 + 0] *= tilingX;
        mesh.texcoords[i * 2 + 1] *= tilingZ;
    }
    UploadMesh(&mesh, false);
    Model model = LoadModelFromMesh(mesh);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;
    return model;
}

// Distance fog + desaturation, applied as a post-process on every draw call while active.
// Depth is recovered from gl_FragCoord.z (always correct, unlike a manually-forwarded world
// position, which raylib only wires up for DrawModel and not for primitives like DrawCube).
static const char* FogVertexShaderCode = R"(
#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragPosition;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragPosition = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char* FogFragmentShaderCode = R"(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragPosition;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 fogColor;
uniform float fogDensity;
uniform float desaturation;
uniform float nearPlane;
uniform float farPlane;
uniform float ambientBrightness;
uniform vec3 viewPos;
uniform float reflectivity;
uniform vec3 puddleLightPos;
#define MAX_LIGHTS 32
uniform vec3 lightPositions[MAX_LIGHTS];
uniform int lightCount;
uniform float lightRadius;
uniform float lightIntensity;
uniform float enableLocalLight;
out vec4 finalColor;
void main()
{
    vec4 texelColor = texture(texture0, fragTexCoord);
    vec4 baseColor = texelColor * fragColor * colDiffuse;

    float ndcZ = gl_FragCoord.z * 2.0 - 1.0;
    float linearDepth = (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - ndcZ * (farPlane - nearPlane));

    float luminance = dot(baseColor.rgb, vec3(0.299, 0.587, 0.114));
    vec3 desaturated = mix(baseColor.rgb, vec3(luminance), desaturation);
    desaturated *= ambientBrightness;

    // Localized streetlight illumination: ambient is kept low overall, and this brightens
    // whatever's near a lamp on top of it - no shadowing, just a per-fragment falloff summed
    // over every lamp, which is cheap enough here that all lights can always contribute.
    // Only enabled for draws where fragPosition is verified correct (DrawModel surfaces and
    // our own explicit-world-coordinate rlgl draws) - raylib's built-in shape primitives
    // (DrawCube, DrawSphere, ...) don't reliably feed matModel, so fragPosition is garbage
    // for them and this must stay off there.
    if (enableLocalLight > 0.5)
    {
        float localLight = 0.0;
        for (int i = 0; i < lightCount; i++)
        {
            float d = distance(fragPosition.xz, lightPositions[i].xz);
            localLight += exp(-(d * d) / (lightRadius * lightRadius));
        }
        vec3 lampColor = vec3(1.0, 0.83, 0.5);
        desaturated += lampColor * min(localLight, 1.0) * lightIntensity;
    }

    // Cheap fake wet-road reflection: no second render pass, just a fresnel-style sheen
    // that brightens toward the sky color at grazing view angles (surface normal is
    // always straight up here, so viewDir.y alone gives the angle to the normal).
    if (reflectivity > 0.0)
    {
        vec3 viewDir = normalize(viewPos - fragPosition);
        float fresnel = pow(clamp(1.0 - viewDir.y, 0.0, 1.0), 1.5);
        // A pure fresnel term all but disappears when looking mostly straight down, which is
        // most of the time in normal play - a baseline keeps the surface visibly reflective
        // (as opposed to just "a dark patch") no matter the viewing angle, with fresnel adding
        // extra punch at grazing ones.
        float reflectAmount = mix(0.4, 1.0, fresnel);
        // A fixed brighter blue-grey rather than a multiple of fogColor - fogColor is very
        // dark in this scene, so scaling it up still reflects as "dark", not "visibly wet".
        vec3 reflectionTint = vec3(0.42, 0.45, 0.52);
        desaturated = mix(desaturated, reflectionTint, reflectAmount * reflectivity);

        // Puddles only (reflectivity is higher there than the plain road): a soft warm
        // blob standing in for the nearest streetlight's blurry reflection. No real
        // reflection texture/render pass, just a distance-based glow in world XZ.
        if (reflectivity > 0.8)
        {
            vec2 delta = fragPosition.xz - puddleLightPos.xz;
            float distSq = dot(delta, delta);
            float glow = exp(-distSq / 14.0);
            vec3 glowColor = vec3(1.0, 0.82, 0.5);
            desaturated += glowColor * glow * 1.3;
        }
    }

    float fogFactor = clamp(1.0 - exp(-fogDensity * linearDepth), 0.0, 1.0);
    vec3 finalRGB = mix(desaturated, fogColor, fogFactor);
    finalColor = vec4(finalRGB, baseColor.a);
}
)";

int main(void)
{
    const int screenWidth = 1280;
    const int screenHeight = 720;

    InitWindow(screenWidth, screenHeight, "Simple 3D Game");
    SetTargetFPS(60);

    // Borderless windowed fullscreen rather than exclusive FLAG_FULLSCREEN_MODE: it just
    // resizes the window to cover the monitor instead of switching the display's video
    // mode, which is both the more standard behavior for a game like this and avoids
    // exclusive-mode quirks on setups (e.g. virtual/remote displays) that don't support it.
    ToggleBorderlessWindowed();

    DisableCursor();

    InitAudioDevice();
    Music rainMusic = LoadMusicStream(TextFormat("%ssound_effects/rain_loop.wav", GetApplicationDirectory()));
    rainMusic.looping = true;
    SetMusicVolume(rainMusic, 0.6f);
    PlayMusicStream(rainMusic);

    // Indoor rain: a separate, more muffled loop swapped in for the outdoor one while inside a
    // house - both streams keep playing continuously in the background and are just faded
    // between via volume, rather than restarted on every enter/exit, to avoid an audible pop.
    Music rainIndoorMusic = LoadMusicStream(TextFormat("%ssound_effects/rain_indoors.mp3", GetApplicationDirectory()));
    rainIndoorMusic.looping = true;
    SetMusicVolume(rainIndoorMusic, 0.0f);
    PlayMusicStream(rainIndoorMusic);

    // Footsteps: started/stopped on demand each frame based on whether the player is currently
    // walking, rather than always streaming like the rain/soundtrack.
    Music footstepsMusic = LoadMusicStream(TextFormat("%ssound_effects/footsteps_loop.wav", GetApplicationDirectory()));
    footstepsMusic.looping = true;
    SetMusicVolume(footstepsMusic, 1.0f);

    // Soundtrack: every .wav in soundtrack/, shuffled, playing one after another on loop
    // for as long as the window stays open.
    std::vector<std::string> soundtrackPaths;
    FilePathList soundtrackFiles = LoadDirectoryFilesEx(TextFormat("%ssoundtrack", GetApplicationDirectory()), ".wav", false);
    for (unsigned int i = 0; i < soundtrackFiles.count; i++) soundtrackPaths.push_back(soundtrackFiles.paths[i]);
    UnloadDirectoryFiles(soundtrackFiles);

    for (int i = (int)soundtrackPaths.size() - 1; i > 0; i--)
    {
        int j = GetRandomValue(0, i);
        std::swap(soundtrackPaths[i], soundtrackPaths[j]);
    }

    const float soundtrackGap = 5.0f;
    int soundtrackIndex = 0;
    bool soundtrackActive = !soundtrackPaths.empty();
    bool soundtrackMusicLoaded = false;
    bool soundtrackWaiting = false;
    float soundtrackPauseTimer = 0.0f;
    Music soundtrackMusic = { 0 };
    if (soundtrackActive)
    {
        soundtrackMusic = LoadMusicStream(soundtrackPaths[0].c_str());
        soundtrackMusic.looping = false;
        SetMusicVolume(soundtrackMusic, 0.5f);
        PlayMusicStream(soundtrackMusic);
        soundtrackMusicLoaded = true;
    }

    const float eyeHeight = 1.8f;

    Camera3D camera = { 0 };
    camera.position = Vector3{ 0.0f, eyeHeight, 0.0f };
    camera.target = Vector3{ 0.0f, eyeHeight, 1.0f };
    camera.up = Vector3{ 0.0f, 1.0f, 0.0f };
    camera.fovy = 70.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    const float moveSpeed = 6.0f;
    const float mouseSensitivity = 0.05f;
    const float gravity = -25.0f;
    const float jumpSpeed = 8.0f;

    float verticalVelocity = 0.0f;
    bool grounded = true;

    // Ground: a recessed grey road between two pavement strips, then green fields beyond.
    // The road and pavement run twice as long as the fields/houses, so they keep going as
    // bare strips into the empty void beyond the built-up part of town.
    const float groundWidth = 200.0f;
    const float groundLength = 400.0f;
    const float roadLength = groundLength * 2.0f;
    const float streetWidth = 16.0f;              // total gap between the two house rows
    const float roadWidth = 10.0f;                // recessed road, centered in that gap
    const float pavementWidth = (streetWidth - roadWidth) / 2.0f;
    const float pavementCenterX = (roadWidth + pavementWidth) / 2.0f;
    const float roadRecess = 0.15f;                // how far the road sits below pavement/field level
    const float curbWidth = 0.2f;
    const float fieldWidth = (groundWidth - streetWidth) / 2.0f;
    const float fieldCenterX = (streetWidth + fieldWidth) / 2.0f;

    const Color roadColor = Color{ 72, 72, 76, 255 };

    Texture2D grassTexture = MakeSpeckleTexture(256,
        Color{ 58, 108, 52, 255 },
        Color{ 52, 100, 48, 255 },
        Color{ 65, 116, 56, 255 },
        Color{ 48, 92, 45, 255 });
    Texture2D pavementTexture = MakeSlabTexture(64, Color{ 150, 150, 147, 255 }, Color{ 108, 108, 106, 255 }, 3);
    Texture2D grateTexture = MakeGrateTexture(32);

    // Road: crowned slightly like a real road (raised center, tapering to the curbs) for
    // drainage. Needs width subdivisions to bend, unlike the other flat ground planes.
    const float roadHumpHeight = 0.08f;
    const int roadHumpSegments = 12;
    Mesh roadMesh = GenMeshPlane(roadWidth, roadLength, roadHumpSegments, 1);
    for (int i = 0; i < roadMesh.vertexCount; i++)
    {
        float x = roadMesh.vertices[i * 3 + 0];
        float t = x / (roadWidth / 2.0f);
        roadMesh.vertices[i * 3 + 1] += roadHumpHeight * (1.0f - t * t);
    }
    UploadMesh(&roadMesh, false);
    Model roadModel = LoadModelFromMesh(roadMesh);

    Model pavementModel = MakeTiledGroundModel(pavementWidth, roadLength, 1.6f, pavementTexture);
    Model fieldModel = MakeTiledGroundModel(fieldWidth, groundLength, 0.5f, grassTexture);

    // Drainage grates: small textured decals laid into the road right against the curb.
    Model grateModel = LoadModelFromMesh(GenMeshPlane(1.0f, 1.4f, 1, 1));
    grateModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = grateTexture;
    std::vector<Vector3> grates;
    const float grateSpacing = 40.0f;
    const float grateInset = 0.7f;
    for (int side = -1; side <= 1; side += 2)
    {
        for (float z = 12.0f; z < roadLength / 2.0f - 5.0f; z += grateSpacing)
        {
            float gx = side * (roadWidth / 2.0f - curbWidth / 2.0f - grateInset);
            grates.push_back(Vector3{ gx, -roadRecess + 0.01f, z });
        }
    }

    // Puddles: irregular blob shapes sitting on the road against the curb, one straight edge
    // where the blob would otherwise cross the curb line. Built as plain triangle fans (no
    // texture needed) so a strong reflectivity value reads as a small mirror-like pool.
    const float curbLineX = roadWidth / 2.0f - curbWidth / 2.0f;
    const int puddleCount = 40;
    const int puddlePointCount = 14;
    const Color puddleColor = Color{ 10, 13, 17, 255 };
    std::vector<Puddle> puddles;
    for (int i = 0; i < puddleCount; i++)
    {
        int side = (GetRandomValue(0, 1) == 0) ? -1 : 1;
        float z = (float)GetRandomValue(-(int)(roadLength / 2.0f) + 5, (int)(roadLength / 2.0f) - 5);
        float baseRadius = (float)GetRandomValue(60, 150) / 100.0f;
        float centerInset = baseRadius * 0.5f;

        Puddle puddle;
        puddle.center = Vector3{ side * (curbLineX - centerInset), -roadRecess + 0.03f, z };

        float phase1 = (float)GetRandomValue(0, 628) / 100.0f;
        float phase2 = (float)GetRandomValue(0, 628) / 100.0f;
        int freq1 = GetRandomValue(2, 4);
        int freq2 = GetRandomValue(3, 6);
        for (int p = 0; p < puddlePointCount; p++)
        {
            float angle = (2.0f * PI * p) / puddlePointCount;
            float bump = 1.0f + 0.35f * sinf(angle * freq1 + phase1) + 0.2f * sinf(angle * freq2 + phase2);
            float radius = baseRadius * bump;

            float x = puddle.center.x + cosf(angle) * radius;
            float pz = puddle.center.z + sinf(angle) * radius;
            x = (side < 0) ? fmaxf(x, -curbLineX) : fminf(x, curbLineX);

            puddle.points.push_back(Vector3{ x, puddle.center.y, pz });
        }
        puddles.push_back(puddle);
    }

    // Paintings for the house interiors, loaded from poster/ (only .png - this raylib build
    // isn't compiled with JPEG support, so the originals were converted to .png; .avif isn't
    // supported by raylib at all and is just skipped by the extension filter).
    std::vector<Texture2D> paintingTextures;
    FilePathList paintingFiles = LoadDirectoryFilesEx(TextFormat("%sposter", GetApplicationDirectory()), ".png", false);
    for (unsigned int i = 0; i < paintingFiles.count; i++)
    {
        Texture2D paintingTexture = LoadTexture(paintingFiles.paths[i]);
        if (paintingTexture.id != 0) paintingTextures.push_back(paintingTexture);
    }
    UnloadDirectoryFiles(paintingFiles);

    // Street of houses: two facing rows of grey cube placeholders running down +z
    std::vector<House> houses;
    const float streetHalfWidth = 12.0f;
    const float houseSpacingZ = 16.0f;
    const float houseStartZ = 14.0f;
    const int housesPerSide = 10;

    // Door/window layout, shared by generation (to know how many floors fit) and drawing.
    const float doorHeight = 2.2f;
    const float windowHeight = 1.3f;
    const float windowWidth = 0.9f;
    const float windowZOffset = 2.2f;
    const float firstWindowY = 3.6f;
    const float floorSpacing = 3.0f;
    const float topMargin = 1.5f;

    for (int side = -1; side <= 1; side += 2)
    {
        for (int i = 0; i < housesPerSide; i++)
        {
            Vector3 size = { 8.0f, GetRandomValue(480, 800) / 10.0f, 8.0f };
            Vector3 position = { side * streetHalfWidth, size.y / 2.0f, houseStartZ + i * houseSpacingZ };
            int shade = GetRandomValue(120, 150);
            Color color = Color{ (unsigned char)shade, (unsigned char)shade, (unsigned char)(shade + 3), 255 };

            int windowRows = (int)((size.y - firstWindowY - topMargin) / floorSpacing) + 1;
            if (windowRows < 1) windowRows = 1;
            std::vector<bool> windowsLit;
            for (int r = 0; r < windowRows; r++)
            {
                windowsLit.push_back(GetRandomValue(0, 99) < 20);
                windowsLit.push_back(GetRandomValue(0, 99) < 20);
            }

            Vector3 doorPos = { position.x - side * (size.x / 2.0f + 0.03f), 0.0f, position.z };
            int paintingIndex = paintingTextures.empty() ? -1 : ((int)houses.size() % (int)paintingTextures.size());

            houses.push_back(House{ position, size, color, side, windowsLit, doorPos, paintingIndex });
        }
    }

    // Trees and fences: occasional, in the gaps between consecutive houses along each row
    // (not out front on the pavement) - each gap gets at most one of the two.
    std::vector<Tree> trees;
    std::vector<Fence> fences;
    const float treeChance = 0.30f;
    const float fenceChance = 0.30f;
    for (int side = -1; side <= 1; side += 2)
    {
        for (int i = 0; i < housesPerSide - 1; i++)
        {
            float gapStartZ = houseStartZ + i * houseSpacingZ + 4.0f;
            float gapEndZ = houseStartZ + (i + 1) * houseSpacingZ - 4.0f;
            float gapCenterZ = (gapStartZ + gapEndZ) / 2.0f;

            int roll = GetRandomValue(0, 99);
            if (roll < (int)(treeChance * 100.0f))
            {
                float trunkHeight = (float)GetRandomValue(600, 1000) / 100.0f;
                float foliageRadius = (float)GetRandomValue(250, 400) / 100.0f;
                trees.push_back(Tree{ Vector3{ (float)side * streetHalfWidth, 0.0f, gapCenterZ }, trunkHeight, foliageRadius });
            }
            else if (roll < (int)((treeChance + fenceChance) * 100.0f))
            {
                fences.push_back(Fence{
                    Vector3{ (float)side * streetHalfWidth, 0.0f, gapStartZ },
                    Vector3{ (float)side * streetHalfWidth, 0.0f, gapEndZ } });
            }
        }
    }

    // Monologue lines, split by the first blank line into two groups: pedestrian lines above
    // it, forgotten-soul lines below it.
    std::vector<std::string> monologueLines;
    std::vector<std::string> forgottenSoulLines;
    std::ifstream monologueFile(TextFormat("%smonologues.txt", GetApplicationDirectory()));
    std::string monologueFileLine;
    bool pastMonologueSeparator = false;
    while (std::getline(monologueFile, monologueFileLine))
    {
        if (monologueFileLine.empty())
        {
            pastMonologueSeparator = true;
            continue;
        }
        if (!pastMonologueSeparator) monologueLines.push_back(monologueFileLine);
        else forgottenSoulLines.push_back(monologueFileLine);
    }
    if (monologueLines.empty()) monologueLines.push_back("Pedestrian: \"...\"");

    // Pedestrians: simple placeholder figures that slowly pace their side of the street and
    // occasionally duck into a house for a while, just to sell the illusion of activity.
    std::vector<Pedestrian> pedestrians;
    const float pedestrianLookRange = 8.0f;
    const float pedestrianLookCos = 0.97f; // ~14 degree half-angle aiming cone
    const float pedestrianSpeed = 1.0f;
    const float pedestrianCorridorMinZ = houseStartZ;
    const float pedestrianCorridorMaxZ = houseStartZ + (housesPerSide - 1) * houseSpacingZ;
    const float doorEnterChance = 0.25f;
    const float doorCheckRadius = 0.6f;
    const float doorTransitionDuration = 1.3f;
    const int pedestrianCount = 5;
    for (int i = 0; i < pedestrianCount; i++)
    {
        int side = (GetRandomValue(0, 1) == 0) ? -1 : 1;
        float z = houseStartZ + (float)GetRandomValue(0, housesPerSide - 1) * houseSpacingZ;
        Pedestrian pedestrian;
        pedestrian.position = Vector3{ side * pavementCenterX, 0.0f, z };
        pedestrian.side = side;
        pedestrian.direction = (GetRandomValue(0, 1) == 0) ? -1.0f : 1.0f;
        pedestrian.state = PED_WALKING;
        pedestrian.stateTimer = 0.0f;
        pedestrian.doorCooldown = 0.0f;
        pedestrian.line = monologueLines[i % monologueLines.size()];
        pedestrians.push_back(pedestrian);
    }

    // Forgotten souls: stationary black-hooded figures placed well past the last house, out in
    // the bare stretch of road/pavement beyond the built-up part of town - meant to be found,
    // not walked past.
    std::vector<ForgottenSoul> forgottenSouls;
    float forgottenSoulZ = pedestrianCorridorMaxZ + 60.0f;
    for (size_t i = 0; i < forgottenSoulLines.size(); i++)
    {
        int side = (i % 2 == 0) ? -1 : 1;
        ForgottenSoul soul;
        soul.position = Vector3{ side * pavementCenterX, 0.0f, forgottenSoulZ };
        soul.line = forgottenSoulLines[i];
        forgottenSouls.push_back(soul);
        forgottenSoulZ += 45.0f;
    }

    // Street lights: upside-down-L posts on the pavement, arm stooping out over the road
    // so the bulb hangs above the street pointing down at it.
    std::vector<StreetLight> streetLights;
    const float lightPoleHeight = 13.0f;
    const float lightArmLength = 3.0f;
    const float lightSpacingZ = 24.0f;
    const float lightStartZ = 8.0f;
    const int lightsPerSide = 14;
    for (int side = -1; side <= 1; side += 2)
    {
        // Offset one side by half the spacing so the lights zigzag down the street
        // instead of standing in facing pairs.
        float sideOffset = (side < 0) ? 0.0f : (lightSpacingZ / 2.0f);
        for (int i = 0; i < lightsPerSide; i++)
        {
            float poleX = side * (roadWidth / 2.0f + 0.3f);
            float z = lightStartZ + sideOffset + i * lightSpacingZ;
            Vector3 poleBase = { poleX, 0.0f, z };
            Vector3 poleTop = { poleX, lightPoleHeight, z };
            Vector3 bulbPosition = { poleX - side * lightArmLength, lightPoleHeight - 0.3f, z };
            streetLights.push_back(StreetLight{ poleBase, poleTop, bulbPosition });
        }
    }

    // Street bins: a few simple ones dotted along the pavement, roughly one every third light.
    std::vector<Bin> bins;
    for (size_t i = 0; i < streetLights.size(); i += 3)
    {
        const StreetLight& light = streetLights[i];
        float side = (light.poleBase.x < 0.0f) ? -1.0f : 1.0f;
        bins.push_back(Bin{ Vector3{ light.poleBase.x + side * 0.45f, 0.0f, light.poleBase.z + 0.5f } });
    }

    // Each puddle remembers the streetlight closest to it, for a cheap stand-in "reflection":
    // a blurry warm glow blob in the puddle shader, rather than a real mirrored render pass.
    for (Puddle& puddle : puddles)
    {
        float bestDistSq = 1e18f;
        Vector3 bestPos = Vector3{ 999999.0f, 0.0f, 999999.0f };
        for (const StreetLight& light : streetLights)
        {
            float dx = puddle.center.x - light.bulbPosition.x;
            float dz = puddle.center.z - light.bulbPosition.z;
            float distSq = dx * dx + dz * dz;
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestPos = light.bulbPosition;
            }
        }
        puddle.nearestLightPos = bestPos;
    }

    // Soft radial glow texture for the light bulbs (a clean halo, unlike the bumpy cloud puffs)
    const int glowTexSize = 64;
    Image glowImage = GenImageColor(glowTexSize, glowTexSize, BLANK);
    Vector2 glowTexCenter = { glowTexSize / 2.0f, glowTexSize / 2.0f };
    for (int y = 0; y < glowTexSize; y++)
    {
        for (int x = 0; x < glowTexSize; x++)
        {
            float dist = Vector2Distance(Vector2{ (float)x, (float)y }, glowTexCenter) / (glowTexSize / 2.0f);
            float falloff = 1.0f - dist;
            if (falloff < 0.0f) falloff = 0.0f;
            unsigned char alpha = (unsigned char)(falloff * falloff * 255.0f);
            ImageDrawPixel(&glowImage, x, y, Color{ 255, 255, 255, alpha });
        }
    }
    Texture2D glowTexture = LoadTextureFromImage(glowImage);
    UnloadImage(glowImage);
    SetTextureFilter(glowTexture, TEXTURE_FILTER_BILINEAR);

    const float lightGlowSize = 2.6f;
    const Color lightGlowColor = Color{ 255, 225, 150, 190 };
    const Color lightPoleColor = Color{ 40, 40, 42, 255 };
    const Color lightBulbColor = Color{ 255, 235, 180, 255 };
    const float poleThickness = 0.1f;
    const float armThickness = 0.08f;
    const float bulbRadius = 0.13f;

    const Color doorColor = Color{ 35, 24, 16, 255 };
    const Color litWindowColor = Color{ 230, 190, 120, 255 };
    const Color darkWindowColor = Color{ 14, 18, 24, 255 };
    const Color treeTrunkColor = Color{ 58, 42, 30, 255 };
    const Color treeFoliageColor = Color{ 26, 48, 24, 255 };
    const Color pedestrianBodyColor = Color{ 50, 58, 72, 255 };
    const Color pedestrianHeadColor = Color{ 205, 175, 150, 255 };
    const Color forgottenSoulColor = Color{ 8, 8, 10, 255 };
    const Color fenceColor = Color{ 70, 68, 64, 255 };
    const Color binColor = Color{ 40, 48, 42, 255 };

    // Cloud puff texture: a soft blob with an irregular, bumpy edge plus internal mottling
    // noise, so it reads as a textured wisp of cloud rather than a flat glowing dot. Alpha
    // is capped well below full opacity so overlapping puffs don't stack into bright patches.
    const int cloudTexSize = 64;
    Image cloudPuffImage = GenImageColor(cloudTexSize, cloudTexSize, BLANK);
    Vector2 cloudTexCenter = { cloudTexSize / 2.0f, cloudTexSize / 2.0f };
    for (int y = 0; y < cloudTexSize; y++)
    {
        for (int x = 0; x < cloudTexSize; x++)
        {
            Vector2 rel = Vector2Subtract(Vector2{ (float)x, (float)y }, cloudTexCenter);
            float dist = Vector2Length(rel) / (cloudTexSize / 2.0f);
            float angle = atan2f(rel.y, rel.x);
            float bump = 0.18f * sinf(angle * 5.0f)
                       + 0.10f * sinf(angle * 9.0f + 1.3f)
                       + 0.06f * sinf(angle * 13.0f + 2.7f);
            float edgeRadius = 1.0f + bump;
            float falloff = (edgeRadius - dist) / edgeRadius;
            if (falloff < 0.0f) falloff = 0.0f;
            if (falloff > 1.0f) falloff = 1.0f;

            float n1 = sinf(x * 0.35f + y * 0.21f);
            float n2 = sinf(x * 0.12f - y * 0.29f + 1.7f);
            float n3 = sinf(x * 0.5f + y * 0.5f + 3.1f);
            float mottle = 0.7f + 0.3f * ((n1 + n2 + n3) / 3.0f);

            float density = falloff * mottle;
            if (density < 0.0f) density = 0.0f;
            if (density > 1.0f) density = 1.0f;
            unsigned char alpha = (unsigned char)(powf(density, 1.6f) * 210.0f);
            ImageDrawPixel(&cloudPuffImage, x, y, Color{ 255, 255, 255, alpha });
        }
    }
    Texture2D cloudTexture = LoadTextureFromImage(cloudPuffImage);
    UnloadImage(cloudPuffImage);
    SetTextureFilter(cloudTexture, TEXTURE_FILTER_BILINEAR);

    // Clouds: dense, tight clumps of overlapping puffs with per-puff shade variation for volume
    std::vector<CloudPuff> clouds;
    const int cloudClumpCount = 12;
    for (int i = 0; i < cloudClumpCount; i++)
    {
        Vector3 clumpCenter = {
            (float)GetRandomValue(-140, 140),
            (float)GetRandomValue(45, 65),
            (float)GetRandomValue(-140, 140)
        };
        int baseShade = GetRandomValue(90, 150);
        int puffsInClump = GetRandomValue(6, 10);
        for (int p = 0; p < puffsInClump; p++)
        {
            Vector3 offset = {
                (float)GetRandomValue(-700, 700) / 100.0f,
                (float)GetRandomValue(-180, 180) / 100.0f,
                (float)GetRandomValue(-700, 700) / 100.0f
            };
            Vector3 puffPosition = Vector3Add(clumpCenter, offset);
            float puffSize = (float)GetRandomValue(1200, 2600) / 100.0f;
            int puffShade = baseShade + GetRandomValue(-20, 20);
            if (puffShade < 60) puffShade = 60;
            if (puffShade > 180) puffShade = 180;
            Color puffColor = Color{ (unsigned char)puffShade, (unsigned char)puffShade, (unsigned char)(puffShade + 5), 150 };
            clouds.push_back(CloudPuff{ puffPosition, puffSize, puffColor });
        }
    }

    // Rain: streaks that fall and respawn above the player so it always rains nearby
    std::vector<Vector3> raindrops;
    const int rainCount = 20000;
    const float rainSpeed = 22.0f;
    const float rainStreakLength = 0.6f;
    const Color rainColor = Color{ 190, 205, 230, 140 };
    for (int i = 0; i < rainCount; i++)
    {
        raindrops.push_back(Vector3{
            (float)GetRandomValue(-3000, 3000) / 100.0f,
            (float)GetRandomValue(0, 3500) / 100.0f,
            (float)GetRandomValue(-3000, 3000) / 100.0f
        });
    }

    // Rain splashes: a small ring that appears where a drop lands and quickly grows/fades.
    // Only a small fraction of the (huge) number of landings actually spawn one, and the
    // pool is capped, so the cost stays flat no matter how dense the rain gets.
    std::vector<RainSplash> splashes;
    const int maxSplashes = 200;
    const float splashChance = 0.02f;
    const float splashLifetime = 0.35f;
    const float splashMaxRadius = 0.35f;
    const Color splashColor = Color{ 205, 220, 235, 255 };

    const Color skyColor = Color{ 45, 46, 50, 255 };

    Shader fogShader = LoadShaderFromMemory(FogVertexShaderCode, FogFragmentShaderCode);
    int fogColorLoc = GetShaderLocation(fogShader, "fogColor");
    int fogDensityLoc = GetShaderLocation(fogShader, "fogDensity");
    int desaturationLoc = GetShaderLocation(fogShader, "desaturation");
    int nearPlaneLoc = GetShaderLocation(fogShader, "nearPlane");
    int farPlaneLoc = GetShaderLocation(fogShader, "farPlane");
    int ambientBrightnessLoc = GetShaderLocation(fogShader, "ambientBrightness");
    int viewPosLoc = GetShaderLocation(fogShader, "viewPos");
    int reflectivityLoc = GetShaderLocation(fogShader, "reflectivity");
    int matModelLoc = GetShaderLocation(fogShader, "matModel");
    int puddleLightPosLoc = GetShaderLocation(fogShader, "puddleLightPos");
    int lightPositionsLoc = GetShaderLocation(fogShader, "lightPositions");
    int lightCountLoc = GetShaderLocation(fogShader, "lightCount");
    int lightRadiusLoc = GetShaderLocation(fogShader, "lightRadius");
    int lightIntensityLoc = GetShaderLocation(fogShader, "lightIntensity");
    int enableLocalLightLoc = GetShaderLocation(fogShader, "enableLocalLight");

    Vector3 fogColorVec = { skyColor.r / 255.0f, skyColor.g / 255.0f, skyColor.b / 255.0f };
    float fogDensity = 0.06f;
    float desaturation = 0.6f;
    float nearPlane = 0.05f;
    float farPlane = 4000.0f;
    float ambientBrightness = 0.55f;
    float lightRadius = 4.5f;
    float lightIntensity = 0.5f;
    const float roadReflectivity = 0.5f;
    const float puddleReflectivity = 0.95f;
    const float noReflectivity = 0.0f;
    const float localLightOn = 1.0f;
    const float localLightOff = 0.0f;
    SetShaderValue(fogShader, fogColorLoc, &fogColorVec, SHADER_UNIFORM_VEC3);
    SetShaderValue(fogShader, fogDensityLoc, &fogDensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, desaturationLoc, &desaturation, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, nearPlaneLoc, &nearPlane, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, farPlaneLoc, &farPlane, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, ambientBrightnessLoc, &ambientBrightness, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, reflectivityLoc, &noReflectivity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, lightRadiusLoc, &lightRadius, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, lightIntensityLoc, &lightIntensity, SHADER_UNIFORM_FLOAT);
    SetShaderValue(fogShader, enableLocalLightLoc, &localLightOff, SHADER_UNIFORM_FLOAT);

    // Streetlights don't move, so their positions only need uploading once.
    std::vector<Vector3> lightPositionsForShader;
    for (const StreetLight& light : streetLights) lightPositionsForShader.push_back(light.bulbPosition);
    int lightCountForShader = (int)lightPositionsForShader.size();
    SetShaderValueV(fogShader, lightPositionsLoc, lightPositionsForShader.data(), SHADER_UNIFORM_VEC3, lightCountForShader);
    SetShaderValue(fogShader, lightCountLoc, &lightCountForShader, SHADER_UNIFORM_INT);

    // DrawModel uses each material's own shader rather than whatever BeginShaderMode has
    // active, so the ground models need the fog shader assigned directly or they're skipped.
    roadModel.materials[0].shader = fogShader;
    pavementModel.materials[0].shader = fogShader;
    fieldModel.materials[0].shader = fogShader;
    grateModel.materials[0].shader = fogShader;

    // Collision: pushes a world XZ position out of anything solid it's overlapping. Each
    // obstacle type is resolved independently (not a single swept move), which is simple
    // rather than physically exact, but is enough to stop the player walking through things.
    const float playerRadius = 0.35f;
    const float poleCollisionRadius = 0.22f;
    const float treeCollisionRadius = 0.35f;
    const float binCollisionRadius = 0.35f;
    const float binClearHeight = 0.5f; // jump above this (feet height off the ground) to clear a bin
    const float fenceHalfThickness = 0.1f;
    auto resolveCollisions = [&](Vector3 pos, float feetHeight) -> Vector3
    {
        for (const House& house : houses)
        {
            float minX = house.position.x - house.size.x / 2.0f - playerRadius;
            float maxX = house.position.x + house.size.x / 2.0f + playerRadius;
            float minZ = house.position.z - house.size.z / 2.0f - playerRadius;
            float maxZ = house.position.z + house.size.z / 2.0f + playerRadius;
            if (pos.x > minX && pos.x < maxX && pos.z > minZ && pos.z < maxZ)
            {
                float penLeft = pos.x - minX;
                float penRight = maxX - pos.x;
                float penBottom = pos.z - minZ;
                float penTop = maxZ - pos.z;
                float minPen = fminf(fminf(penLeft, penRight), fminf(penBottom, penTop));
                if (minPen == penLeft) pos.x = minX;
                else if (minPen == penRight) pos.x = maxX;
                else if (minPen == penBottom) pos.z = minZ;
                else pos.z = maxZ;
            }
        }

        auto pushFromCircle = [&](Vector3 obstacle, float obstacleRadius)
        {
            float dx = pos.x - obstacle.x;
            float dz = pos.z - obstacle.z;
            float distSq = dx * dx + dz * dz;
            float minDist = playerRadius + obstacleRadius;
            if (distSq < minDist * minDist)
            {
                float dist = sqrtf(distSq);
                if (dist < 0.0001f) { pos.x += minDist; return; }
                float push = minDist - dist;
                pos.x += (dx / dist) * push;
                pos.z += (dz / dist) * push;
            }
        };

        for (const StreetLight& light : streetLights) pushFromCircle(light.poleBase, poleCollisionRadius);
        for (const Tree& tree : trees) pushFromCircle(tree.position, treeCollisionRadius);
        if (feetHeight < binClearHeight)
        {
            for (const Bin& bin : bins) pushFromCircle(bin.position, binCollisionRadius);
        }

        for (const Fence& fence : fences)
        {
            float fx = fence.start.x;
            float minZ = fminf(fence.start.z, fence.end.z) - playerRadius;
            float maxZ = fmaxf(fence.start.z, fence.end.z) + playerRadius;
            float minX = fx - fenceHalfThickness - playerRadius;
            float maxX = fx + fenceHalfThickness + playerRadius;
            if (pos.x > minX && pos.x < maxX && pos.z > minZ && pos.z < maxZ)
            {
                float penLeft = pos.x - minX;
                float penRight = maxX - pos.x;
                if (penLeft < penRight) pos.x = minX; else pos.x = maxX;
            }
        }

        return pos;
    };

    // A single shared "simple box for now" interior that every house's door leads into. Which
    // painting hangs on the far wall, and which exterior spot "outside" leads back to, both
    // change per house - only the room shape itself is reused.
    const Vector3 interiorCenter = Vector3{ 5000.0f, 0.0f, 0.0f };
    const float interiorHalfSize = 3.0f;
    const float interiorHeight = 3.2f;
    const Color interiorFloorColor = Color{ 90, 70, 55, 255 };
    const Color interiorWallColor = Color{ 130, 120, 110, 255 };

    // Painting on the wall facing the entrance - a textured quad standing on end, swapped to
    // whichever poster the current house was assigned. Falls back to a plain frame+circle if
    // no paintings loaded.
    const Vector3 portraitCenter = Vector3{ interiorCenter.x, interiorHeight * 0.55f, interiorCenter.z + interiorHalfSize - 0.15f };
    const Color portraitFrameColor = Color{ 222, 203, 170, 255 };
    const Color portraitCircleColor = WHITE;
    Model paintingModel = LoadModelFromMesh(GenMeshPlane(1.1f, 1.4f, 1, 1));
    paintingModel.materials[0].shader = fogShader;

    // A door on the entrance wall, matching the exterior door's look - the player spawns just
    // in front of it, facing into the room.
    const Vector3 interiorDoorCenter = Vector3{ interiorCenter.x, doorHeight / 2.0f, interiorCenter.z - interiorHalfSize + 0.11f };
    const Vector3 interiorSpawnPos = Vector3{ interiorCenter.x, eyeHeight, interiorCenter.z - interiorHalfSize + 1.5f };

    // Outlooking windows on the side walls: a glass-tinted decal plus a few static rain
    // streaks just in front of it - not a real view outside (the interior isn't anywhere near
    // the actual street), just enough to suggest one through the glass.
    const float interiorWindowY = interiorHeight * 0.6f;
    const float interiorWindowSize = 1.1f;
    const Color interiorGlassColor = Color{ 40, 46, 56, 220 };
    const Color interiorRainColor = Color{ 170, 185, 210, 160 };

    const float interactRange = 4.0f;
    const float interactCos = 0.94f; // a bit more forgiving than the old look-straight-at check

    bool inDialogue = false;
    bool inInterior = false;
    std::string dialogueName;
    std::string dialogueText;
    int dialoguePedestrianIndex = -1; // which pedestrian (if any) to freeze while talking to them
    int currentPaintingIndex = -1;    // which poster to show inside, set on entry to match the house
    Vector3 exteriorReturnPos = camera.position;
    Vector3 exteriorReturnTarget = camera.target;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();

        UpdateMusicStream(rainMusic);
        UpdateMusicStream(rainIndoorMusic);
        SetMusicVolume(rainMusic, inInterior ? 0.0f : 0.6f);
        SetMusicVolume(rainIndoorMusic, inInterior ? 0.6f : 0.0f);

        if (soundtrackActive)
        {
            if (soundtrackWaiting)
            {
                soundtrackPauseTimer -= dt;
                if (soundtrackPauseTimer <= 0.0f)
                {
                    soundtrackMusic = LoadMusicStream(soundtrackPaths[soundtrackIndex].c_str());
                    soundtrackMusic.looping = false;
                    SetMusicVolume(soundtrackMusic, 0.5f);
                    PlayMusicStream(soundtrackMusic);
                    soundtrackMusicLoaded = true;
                    soundtrackWaiting = false;
                }
            }
            else
            {
                UpdateMusicStream(soundtrackMusic);
                if (!IsMusicStreamPlaying(soundtrackMusic))
                {
                    UnloadMusicStream(soundtrackMusic);
                    soundtrackMusicLoaded = false;
                    soundtrackIndex++;
                    if (soundtrackIndex >= (int)soundtrackPaths.size())
                    {
                        soundtrackIndex = 0;
                        for (int i = (int)soundtrackPaths.size() - 1; i > 0; i--)
                        {
                            int j = GetRandomValue(0, i);
                            std::swap(soundtrackPaths[i], soundtrackPaths[j]);
                        }
                    }
                    soundtrackWaiting = true;
                    soundtrackPauseTimer = soundtrackGap;
                }
            }
        }

        for (size_t pedIndex = 0; pedIndex < pedestrians.size(); pedIndex++)
        {
            if (inDialogue && (int)pedIndex == dialoguePedestrianIndex) continue;

            Pedestrian& pedestrian = pedestrians[pedIndex];
            pedestrian.doorCooldown -= dt;

            if (pedestrian.state == PED_WALKING)
            {
                pedestrian.position.z += pedestrian.direction * pedestrianSpeed * dt;
                if (pedestrian.position.z > pedestrianCorridorMaxZ)
                {
                    pedestrian.position.z = pedestrianCorridorMaxZ;
                    pedestrian.direction = -1.0f;
                }
                if (pedestrian.position.z < pedestrianCorridorMinZ)
                {
                    pedestrian.position.z = pedestrianCorridorMinZ;
                    pedestrian.direction = 1.0f;
                }

                if (pedestrian.doorCooldown <= 0.0f)
                {
                    for (const House& house : houses)
                    {
                        if (house.side != pedestrian.side) continue;
                        if (fabsf(house.position.z - pedestrian.position.z) >= doorCheckRadius) continue;

                        pedestrian.doorCooldown = 4.0f;
                        if (GetRandomValue(0, 99) < (int)(doorEnterChance * 100.0f))
                        {
                            pedestrian.doorPos = house.doorPos;
                            pedestrian.moveStartPos = pedestrian.position;
                            pedestrian.state = PED_ENTERING;
                            pedestrian.stateTimer = 0.0f;
                        }
                        break;
                    }
                }
            }
            else if (pedestrian.state == PED_ENTERING)
            {
                pedestrian.stateTimer += dt;
                float t = Clamp(pedestrian.stateTimer / doorTransitionDuration, 0.0f, 1.0f);
                pedestrian.position = Vector3Lerp(pedestrian.moveStartPos, pedestrian.doorPos, t);
                if (t >= 1.0f)
                {
                    pedestrian.state = PED_INSIDE;
                    pedestrian.stateTimer = (float)GetRandomValue(600, 1400) / 100.0f;
                }
            }
            else if (pedestrian.state == PED_INSIDE)
            {
                pedestrian.stateTimer -= dt;
                if (pedestrian.stateTimer <= 0.0f)
                {
                    pedestrian.moveStartPos = pedestrian.doorPos;
                    pedestrian.state = PED_EXITING;
                    pedestrian.stateTimer = 0.0f;
                }
            }
            else if (pedestrian.state == PED_EXITING)
            {
                pedestrian.stateTimer += dt;
                float t = Clamp(pedestrian.stateTimer / doorTransitionDuration, 0.0f, 1.0f);
                Vector3 pavementReturn = Vector3{ pedestrian.side * pavementCenterX, 0.0f, pedestrian.doorPos.z };
                pedestrian.position = Vector3Lerp(pedestrian.moveStartPos, pavementReturn, t);
                if (t >= 1.0f)
                {
                    pedestrian.state = PED_WALKING;
                    pedestrian.direction = (GetRandomValue(0, 1) == 0) ? -1.0f : 1.0f;
                }
            }
        }

        // Always drained (even mid-dialogue) so a big stale delta doesn't jump the view once
        // movement resumes.
        Vector2 mouseDelta = GetMouseDelta();

        if (!inDialogue)
        {
            if (grounded && IsKeyPressed(KEY_SPACE))
            {
                verticalVelocity = jumpSpeed;
                grounded = false;
            }

            verticalVelocity += gravity * dt;

            Vector3 movement = {
                ((float)IsKeyDown(KEY_W) - (float)IsKeyDown(KEY_S)) * moveSpeed * dt,
                ((float)IsKeyDown(KEY_D) - (float)IsKeyDown(KEY_A)) * moveSpeed * dt,
                verticalVelocity * dt
            };

            Vector3 rotation = {
                mouseDelta.x * mouseSensitivity,
                mouseDelta.y * mouseSensitivity,
                0.0f
            };

            UpdateCameraPro(&camera, movement, rotation, 0.0f);

            if (camera.position.y < eyeHeight)
            {
                float diff = eyeHeight - camera.position.y;
                camera.position.y += diff;
                camera.target.y += diff;
                verticalVelocity = 0.0f;
                grounded = true;
            }

            if (inInterior)
            {
                camera.position.x = Clamp(camera.position.x, interiorCenter.x - interiorHalfSize + playerRadius, interiorCenter.x + interiorHalfSize - playerRadius);
                camera.position.z = Clamp(camera.position.z, interiorCenter.z - interiorHalfSize + playerRadius, interiorCenter.z + interiorHalfSize - playerRadius);
            }
            else
            {
                float feetHeight = camera.position.y - eyeHeight;
                Vector3 resolved = resolveCollisions(Vector3{ camera.position.x, 0.0f, camera.position.z }, feetHeight);
                float dx = resolved.x - camera.position.x;
                float dz = resolved.z - camera.position.z;
                camera.position.x += dx;
                camera.position.z += dz;
                camera.target.x += dx;
                camera.target.z += dz;
            }
        }

        // Footsteps: on the instant WASD is held while grounded, off the instant it isn't -
        // no fade, matching a plain walk-cycle loop rather than a per-step trigger.
        bool wasdHeld = IsKeyDown(KEY_W) || IsKeyDown(KEY_S) || IsKeyDown(KEY_A) || IsKeyDown(KEY_D);
        bool isWalking = wasdHeld && grounded && !inDialogue;
        if (isWalking && !IsMusicStreamPlaying(footstepsMusic)) PlayMusicStream(footstepsMusic);
        else if (!isWalking && IsMusicStreamPlaying(footstepsMusic)) StopMusicStream(footstepsMusic);
        UpdateMusicStream(footstepsMusic);

        // Frozen in place while indoors, rather than respawning around the interior's own
        // (very much offstage) coordinates - they'll pick back up around the player as soon
        // as they step back outside.
        if (!inInterior)
        {
            for (Vector3& drop : raindrops)
            {
                drop.y -= rainSpeed * dt;
                if (drop.y < 0.0f)
                {
                    if (splashes.size() < (size_t)maxSplashes && GetRandomValue(0, 999) < (int)(splashChance * 1000.0f))
                    {
                        splashes.push_back(RainSplash{ Vector3{ drop.x, 0.0f, drop.z }, 0.0f });
                    }

                    drop.x = camera.position.x + (float)GetRandomValue(-3000, 3000) / 100.0f;
                    drop.z = camera.position.z + (float)GetRandomValue(-3000, 3000) / 100.0f;
                    drop.y = camera.position.y + (float)GetRandomValue(2000, 3500) / 100.0f;
                }
            }
        }

        for (size_t i = 0; i < splashes.size();)
        {
            splashes[i].age += dt;
            if (splashes[i].age >= splashLifetime) splashes.erase(splashes.begin() + i);
            else i++;
        }

        SetShaderValue(fogShader, viewPosLoc, &camera.position, SHADER_UNIFORM_VEC3);

        // What's the player currently able to interact with? Angle+range checks against a
        // rough chest/doorway height, no raycast/occlusion test - same style as the old
        // auto-popup look-at check, just now gated behind pressing E instead of firing on
        // sight, and covering doors as well as people.
        enum { INTERACT_NONE, INTERACT_TALK, INTERACT_ENTER, INTERACT_EXIT };
        int interactionType = INTERACT_NONE;
        std::string interactionName;
        std::string interactionLine;
        std::string interactionPrompt;
        int interactionPedestrianIndex = -1; // which pedestrian, if INTERACT_TALK came from one (not a forgotten soul)
        int interactionHouseIndex = -1;      // which house, if INTERACT_ENTER
        Vector3 cameraForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));

        if (!inDialogue)
        {
            if (inInterior)
            {
                // Uses the camera's own height rather than a fixed chest height: the room is
                // small enough that a fixed reference point creates a steep, easily-missed
                // look-angle at typical indoor distances (unlike the more distant outdoor
                // door/pedestrian checks, where that vertical offset barely matters).
                Vector3 doorChest = { interiorDoorCenter.x, camera.position.y, interiorDoorCenter.z };
                Vector3 toDoor = Vector3Subtract(doorChest, camera.position);
                float doorDist = Vector3Length(toDoor);
                if (doorDist < interactRange && Vector3DotProduct(cameraForward, Vector3Scale(toDoor, 1.0f / doorDist)) > interactCos)
                {
                    interactionType = INTERACT_EXIT;
                    interactionPrompt = "Press E to leave";
                }
            }
            else
            {
                for (size_t pi = 0; pi < pedestrians.size(); pi++)
                {
                    const Pedestrian& pedestrian = pedestrians[pi];
                    if (pedestrian.state == PED_INSIDE) continue;
                    Vector3 chest = { pedestrian.position.x, 1.1f, pedestrian.position.z };
                    Vector3 toTarget = Vector3Subtract(chest, camera.position);
                    float dist = Vector3Length(toTarget);
                    if (dist < pedestrianLookRange && Vector3DotProduct(cameraForward, Vector3Scale(toTarget, 1.0f / dist)) > pedestrianLookCos)
                    {
                        std::string name, quote;
                        ParseDialogueLine(pedestrian.line, name, quote);
                        interactionType = INTERACT_TALK;
                        interactionName = name;
                        interactionLine = pedestrian.line;
                        interactionPrompt = name + " - Press E to talk";
                        interactionPedestrianIndex = (int)pi;
                        break;
                    }
                }

                if (interactionType == INTERACT_NONE)
                {
                    for (const ForgottenSoul& soul : forgottenSouls)
                    {
                        Vector3 chest = { soul.position.x, 1.1f, soul.position.z };
                        Vector3 toTarget = Vector3Subtract(chest, camera.position);
                        float dist = Vector3Length(toTarget);
                        if (dist < pedestrianLookRange && Vector3DotProduct(cameraForward, Vector3Scale(toTarget, 1.0f / dist)) > pedestrianLookCos)
                        {
                            std::string name, quote;
                            ParseDialogueLine(soul.line, name, quote);
                            interactionType = INTERACT_TALK;
                            interactionName = name;
                            interactionLine = soul.line;
                            interactionPrompt = name + " - Press E to talk";
                            break;
                        }
                    }
                }

                if (interactionType == INTERACT_NONE)
                {
                    for (size_t hi = 0; hi < houses.size(); hi++)
                    {
                        const House& house = houses[hi];
                        Vector3 doorChest = { house.doorPos.x, 1.1f, house.doorPos.z };
                        Vector3 toTarget = Vector3Subtract(doorChest, camera.position);
                        float dist = Vector3Length(toTarget);
                        if (dist < interactRange && Vector3DotProduct(cameraForward, Vector3Scale(toTarget, 1.0f / dist)) > interactCos)
                        {
                            interactionType = INTERACT_ENTER;
                            interactionPrompt = "Press E to enter";
                            interactionHouseIndex = (int)hi;
                            break;
                        }
                    }
                }
            }
        }

        if (IsKeyPressed(KEY_E))
        {
            if (interactionType == INTERACT_TALK)
            {
                inDialogue = true;
                dialogueName = interactionName;
                dialoguePedestrianIndex = interactionPedestrianIndex;
                ParseDialogueLine(interactionLine, dialogueName, dialogueText);
            }
            else if (interactionType == INTERACT_ENTER)
            {
                exteriorReturnPos = camera.position;
                exteriorReturnTarget = camera.target;
                camera.position = interiorSpawnPos;
                camera.target = Vector3{ interiorSpawnPos.x, interiorSpawnPos.y, interiorSpawnPos.z + 1.0f };
                inInterior = true;
                currentPaintingIndex = (interactionHouseIndex >= 0) ? houses[interactionHouseIndex].paintingIndex : -1;
            }
            else if (interactionType == INTERACT_EXIT)
            {
                // Same spot they entered from, but facing away from the house rather than back
                // toward the door they just walked through.
                Vector3 entryForward = Vector3Normalize(Vector3Subtract(exteriorReturnTarget, exteriorReturnPos));
                camera.position = exteriorReturnPos;
                camera.target = Vector3Subtract(exteriorReturnPos, entryForward);
                inInterior = false;
            }
        }

        if (inDialogue)
        {
            if (IsKeyPressed(KEY_ONE)) dialogueText = "\"It's been raining for as long as anyone can remember.\"";
            else if (IsKeyPressed(KEY_TWO)) dialogueText = "\"I am " + dialogueName + ". That is all you need to know.\"";
            else if (IsKeyPressed(KEY_THREE) || IsKeyPressed(KEY_ESCAPE))
            {
                inDialogue = false;
                dialoguePedestrianIndex = -1;
            }
        }

        BeginDrawing();
            ClearBackground(skyColor);

            BeginMode3D(camera);
                BeginShaderMode(fogShader);
                    for (const CloudPuff& puff : clouds) DrawBillboard(camera, cloudTexture, puff.position, puff.size, puff.color);

                    // Reflectivity and the localized streetlight glow are both driven by
                    // fragPosition, which is only reliable for DrawModel calls and our own
                    // explicit-world-coordinate rlgl draws (puddles, rain) - left on for that
                    // whole stretch, then switched off before the primitive-drawn scenery.
                    SetShaderValue(fogShader, enableLocalLightLoc, &localLightOn, SHADER_UNIFORM_FLOAT);
                    SetShaderValue(fogShader, reflectivityLoc, &roadReflectivity, SHADER_UNIFORM_FLOAT);
                    DrawModel(roadModel, Vector3{ 0.0f, -roadRecess, 0.0f }, 1.0f, roadColor);
                    SetShaderValue(fogShader, reflectivityLoc, &noReflectivity, SHADER_UNIFORM_FLOAT);

                    // Puddles: plain rlgl triangle fans, so matModel needs resetting to identity
                    // first since their points are already absolute world coordinates (DrawModel
                    // above left matModel set to the road's own transform). Each triangle is
                    // emitted with both windings so it renders regardless of the fan's winding
                    // direction or backface-culling state - simpler and more robust than trying
                    // to toggle rlDisableBackfaceCulling around rlgl's deferred/batched draws.
                    SetShaderValueMatrix(fogShader, matModelLoc, MatrixIdentity());
                    SetShaderValue(fogShader, reflectivityLoc, &puddleReflectivity, SHADER_UNIFORM_FLOAT);
                    for (const Puddle& puddle : puddles)
                    {
                        SetShaderValue(fogShader, puddleLightPosLoc, &puddle.nearestLightPos, SHADER_UNIFORM_VEC3);
                        rlBegin(RL_TRIANGLES);
                            rlColor4ub(puddleColor.r, puddleColor.g, puddleColor.b, puddleColor.a);
                            for (size_t p = 0; p < puddle.points.size(); p++)
                            {
                                const Vector3& a = puddle.points[p];
                                const Vector3& b = puddle.points[(p + 1) % puddle.points.size()];
                                rlVertex3f(puddle.center.x, puddle.center.y, puddle.center.z);
                                rlVertex3f(a.x, a.y, a.z);
                                rlVertex3f(b.x, b.y, b.z);

                                rlVertex3f(puddle.center.x, puddle.center.y, puddle.center.z);
                                rlVertex3f(b.x, b.y, b.z);
                                rlVertex3f(a.x, a.y, a.z);
                            }
                        rlEnd();
                    }
                    SetShaderValue(fogShader, reflectivityLoc, &noReflectivity, SHADER_UNIFORM_FLOAT);

                    DrawModel(pavementModel, Vector3{ -pavementCenterX, 0.0f, 0.0f }, 1.0f, WHITE);
                    DrawModel(pavementModel, Vector3{ pavementCenterX, 0.0f, 0.0f }, 1.0f, WHITE);
                    DrawModel(fieldModel, Vector3{ -fieldCenterX, 0.0f, 0.0f }, 1.0f, WHITE);
                    DrawModel(fieldModel, Vector3{ fieldCenterX, 0.0f, 0.0f }, 1.0f, WHITE);

                    for (const Vector3& gratePos : grates) DrawModel(grateModel, gratePos, 1.0f, WHITE);

                    // Everything below here (until the rain lines) is drawn with raylib's own
                    // primitive calls (DrawCube, DrawSphere, ...), whose fragPosition can't be
                    // trusted (confirmed by testing - a pedestrian far from every light still lit
                    // up), so the local light glow is switched off for this stretch.
                    SetShaderValueMatrix(fogShader, matModelLoc, MatrixIdentity());
                    SetShaderValue(fogShader, enableLocalLightLoc, &localLightOff, SHADER_UNIFORM_FLOAT);

                    // Inset half a curb-width from the road edge so its top face doesn't overlap
                    // the pavement plane (both sit at y=0) - that coincident overlap was causing
                    // the z-fighting flicker on the pavement as the camera moved.
                    DrawCube(Vector3{ -(roadWidth / 2.0f - curbWidth / 2.0f), -roadRecess / 2.0f, 0.0f }, curbWidth, roadRecess, roadLength, LIGHTGRAY);
                    DrawCube(Vector3{ (roadWidth / 2.0f - curbWidth / 2.0f), -roadRecess / 2.0f, 0.0f }, curbWidth, roadRecess, roadLength, LIGHTGRAY);

                    for (const House& house : houses)
                    {
                        DrawCube(house.position, house.size.x, house.size.y, house.size.z, house.color);
                        DrawCubeWires(house.position, house.size.x, house.size.y, house.size.z, DARKGRAY);

                        // Door + one row of windows per floor, all the way up to the top of the
                        // building. Placement is relative to the player's own height (door,
                        // first floor) rather than the building's own, often much taller, scale.
                        float faceX = house.doorPos.x;

                        DrawCube(Vector3{ faceX, doorHeight / 2.0f, house.position.z }, 0.06f, doorHeight, 1.0f, doorColor);

                        int windowRows = (int)house.windowsLit.size() / 2;
                        for (int r = 0; r < windowRows; r++)
                        {
                            float wy = firstWindowY + r * floorSpacing;
                            Color win1Color = house.windowsLit[r * 2] ? litWindowColor : darkWindowColor;
                            Color win2Color = house.windowsLit[r * 2 + 1] ? litWindowColor : darkWindowColor;
                            DrawCube(Vector3{ faceX, wy, house.position.z - windowZOffset }, 0.06f, windowHeight, windowWidth, win1Color);
                            DrawCube(Vector3{ faceX, wy, house.position.z + windowZOffset }, 0.06f, windowHeight, windowWidth, win2Color);
                        }
                    }

                    for (const Fence& fence : fences)
                    {
                        float length = fence.end.z - fence.start.z;
                        int postCount = (int)(length / 1.5f) + 1;
                        for (int p = 0; p <= postCount; p++)
                        {
                            float t = (float)p / (float)postCount;
                            float z = fence.start.z + t * length;
                            DrawCylinder(Vector3{ fence.start.x, 0.0f, z }, 0.03f, 0.03f, 1.1f, 6, fenceColor);
                        }
                        DrawLine3D(Vector3{ fence.start.x, 0.35f, fence.start.z }, Vector3{ fence.start.x, 0.35f, fence.end.z }, fenceColor);
                        DrawLine3D(Vector3{ fence.start.x, 0.7f, fence.start.z }, Vector3{ fence.start.x, 0.7f, fence.end.z }, fenceColor);
                        DrawLine3D(Vector3{ fence.start.x, 1.05f, fence.start.z }, Vector3{ fence.start.x, 1.05f, fence.end.z }, fenceColor);
                    }

                    for (const Bin& bin : bins)
                    {
                        DrawCylinder(bin.position, 0.28f, 0.22f, 0.6f, 10, binColor);
                        DrawCylinderWires(bin.position, 0.28f, 0.22f, 0.6f, 10, DARKGRAY);
                    }

                    // A single shared "simple box for now" interior every house's door leads
                    // into - always drawn, but far enough from the main street that it never
                    // shows up unless the player has actually been teleported inside it.
                    DrawCube(Vector3{ interiorCenter.x, 0.0f, interiorCenter.z }, interiorHalfSize * 2.0f, 0.2f, interiorHalfSize * 2.0f, interiorFloorColor);
                    DrawCube(Vector3{ interiorCenter.x, interiorHeight, interiorCenter.z }, interiorHalfSize * 2.0f, 0.2f, interiorHalfSize * 2.0f, interiorWallColor);
                    DrawCube(Vector3{ interiorCenter.x - interiorHalfSize, interiorHeight / 2.0f, interiorCenter.z }, 0.2f, interiorHeight, interiorHalfSize * 2.0f, interiorWallColor);
                    DrawCube(Vector3{ interiorCenter.x + interiorHalfSize, interiorHeight / 2.0f, interiorCenter.z }, 0.2f, interiorHeight, interiorHalfSize * 2.0f, interiorWallColor);
                    DrawCube(Vector3{ interiorCenter.x, interiorHeight / 2.0f, interiorCenter.z - interiorHalfSize }, interiorHalfSize * 2.0f, interiorHeight, 0.2f, interiorWallColor);
                    DrawCube(Vector3{ interiorCenter.x, interiorHeight / 2.0f, interiorCenter.z + interiorHalfSize }, interiorHalfSize * 2.0f, interiorHeight, 0.2f, interiorWallColor);

                    // Door on the entrance wall (behind the player on entry), matching the
                    // exterior door's look.
                    DrawCube(interiorDoorCenter, 1.0f, doorHeight, 0.06f, doorColor);

                    // Painting: the actual poster texture for this house if one loaded, else a
                    // plain placeholder frame+circle. The quad is a horizontal plane stood up
                    // on end via rotation, so backface culling is disabled around it rather
                    // than guess which rotation sign faces the room (unlike the batched
                    // immediate-mode draws elsewhere, toggling culling around a single mesh
                    // draw call like this is reliable - it's not deferred/batched).
                    if (currentPaintingIndex >= 0 && currentPaintingIndex < (int)paintingTextures.size())
                    {
                        paintingModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = paintingTextures[currentPaintingIndex];
                        rlDisableBackfaceCulling();
                        DrawModelEx(paintingModel, portraitCenter, Vector3{ 1.0f, 0.0f, 0.0f }, 90.0f, Vector3{ -1.0f, 1.0f, 1.0f }, WHITE);
                        rlEnableBackfaceCulling();
                    }
                    else
                    {
                        DrawCube(portraitCenter, 1.0f, 1.3f, 0.04f, portraitFrameColor);
                        DrawSphere(Vector3{ portraitCenter.x, portraitCenter.y, portraitCenter.z - 0.08f }, 0.25f, portraitCircleColor);
                    }

                    // Outlooking windows: a tinted glass decal on each side wall, with a few
                    // static rain streaks just in front for the illusion of weather outside -
                    // not a real view (the interior isn't anywhere near the actual street).
                    {
                        Vector3 westWindow = { interiorCenter.x - interiorHalfSize + 0.11f, interiorWindowY, interiorCenter.z };
                        Vector3 eastWindow = { interiorCenter.x + interiorHalfSize - 0.11f, interiorWindowY, interiorCenter.z };
                        DrawCube(westWindow, 0.04f, interiorWindowSize, interiorWindowSize, interiorGlassColor);
                        DrawCube(eastWindow, 0.04f, interiorWindowSize, interiorWindowSize, interiorGlassColor);
                        for (int wr = -1; wr <= 1; wr++)
                        {
                            float wz = interiorCenter.z + wr * 0.3f;
                            DrawLine3D(Vector3{ westWindow.x - 0.08f, interiorWindowY + 0.4f, wz }, Vector3{ westWindow.x - 0.08f, interiorWindowY - 0.4f, wz }, interiorRainColor);
                            DrawLine3D(Vector3{ eastWindow.x + 0.08f, interiorWindowY + 0.4f, wz }, Vector3{ eastWindow.x + 0.08f, interiorWindowY - 0.4f, wz }, interiorRainColor);
                        }
                    }

                    for (const Tree& tree : trees)
                    {
                        DrawCylinder(tree.position, 0.2f, 0.25f, tree.trunkHeight, 8, treeTrunkColor);
                        Vector3 foliageCenter = { tree.position.x, tree.position.y + tree.trunkHeight + tree.foliageRadius * 0.6f, tree.position.z };
                        DrawSphere(foliageCenter, tree.foliageRadius, treeFoliageColor);
                    }

                    for (const Pedestrian& pedestrian : pedestrians)
                    {
                        if (pedestrian.state == PED_INSIDE) continue;

                        Vector3 feet = { pedestrian.position.x, 0.3f, pedestrian.position.z };
                        Vector3 shoulders = { pedestrian.position.x, 1.4f, pedestrian.position.z };
                        DrawCapsule(feet, shoulders, 0.22f, 8, 4, pedestrianBodyColor);
                        DrawSphere(Vector3{ pedestrian.position.x, 1.62f, pedestrian.position.z }, 0.16f, pedestrianHeadColor);
                    }

                    for (const ForgottenSoul& soul : forgottenSouls)
                    {
                        Vector3 feet = { soul.position.x, 0.3f, soul.position.z };
                        Vector3 shoulders = { soul.position.x, 1.5f, soul.position.z };
                        DrawCapsule(feet, shoulders, 0.24f, 8, 4, forgottenSoulColor);
                        DrawSphere(Vector3{ soul.position.x, 1.75f, soul.position.z }, 0.17f, forgottenSoulColor);
                    }

                    for (const StreetLight& light : streetLights)
                    {
                        Vector3 poleCenter = { light.poleBase.x, (light.poleBase.y + light.poleTop.y) / 2.0f, light.poleBase.z };
                        float poleHeight = light.poleTop.y - light.poleBase.y;
                        DrawCube(poleCenter, poleThickness, poleHeight, poleThickness, lightPoleColor);

                        Vector3 armCenter = { (light.poleTop.x + light.bulbPosition.x) / 2.0f, light.poleTop.y, light.poleTop.z };
                        float armLength = fabsf(light.bulbPosition.x - light.poleTop.x);
                        DrawCube(armCenter, armLength, armThickness, armThickness, lightPoleColor);

                        DrawSphere(light.bulbPosition, bulbRadius, lightBulbColor);
                    }

                    BeginBlendMode(BLEND_ADDITIVE);
                        for (const StreetLight& light : streetLights)
                        {
                            DrawBillboard(camera, glowTexture, light.bulbPosition, lightGlowSize, lightGlowColor);
                        }
                    EndBlendMode();

                    // Batched into a single rlBegin/rlEnd pair instead of one DrawLine3D call
                    // per drop (which each open/close their own batch) - lets the drop count
                    // scale up cheaply since it's just more vertices in one draw, not more calls.
                    // Like puddles, these vertices are our own explicit world coordinates fed
                    // through an identity matModel, so the local light glow is reliable here -
                    // this is what makes raindrops brighten as they pass near a streetlight.
                    // Skipped entirely indoors - it's an interior, it shouldn't be raining there.
                    if (!inInterior)
                    {
                        SetShaderValue(fogShader, enableLocalLightLoc, &localLightOn, SHADER_UNIFORM_FLOAT);
                        rlBegin(RL_LINES);
                            rlColor4ub(rainColor.r, rainColor.g, rainColor.b, rainColor.a);
                            for (const Vector3& drop : raindrops)
                            {
                                rlVertex3f(drop.x, drop.y, drop.z);
                                rlVertex3f(drop.x, drop.y - rainStreakLength, drop.z);
                            }
                        rlEnd();
                        SetShaderValue(fogShader, enableLocalLightLoc, &localLightOff, SHADER_UNIFORM_FLOAT);
                    }

                    for (const RainSplash& splash : splashes)
                    {
                        float t = splash.age / splashLifetime;
                        float radius = splashMaxRadius * t;
                        Color c = splashColor;
                        c.a = (unsigned char)(255.0f * (1.0f - t));
                        DrawCircle3D(splash.position, radius, Vector3{ 1.0f, 0.0f, 0.0f }, 90.0f, c);
                    }
                EndShaderMode();
            EndMode3D();

            DrawText("WASD move | Mouse look | SPACE jump | ESC quit", 10, 10, 20, RAYWHITE);

            int screenW = GetScreenWidth();
            int screenH = GetScreenHeight();

            if (!inDialogue)
            {
                // A small crosshair so aiming at people/doors is legible, plus the hover prompt.
                DrawCircle(screenW / 2, screenH / 2, 2.0f, RAYWHITE);
                if (interactionType != INTERACT_NONE)
                {
                    int promptFontSize = 18;
                    int promptWidth = MeasureText(interactionPrompt.c_str(), promptFontSize);
                    DrawRectangle(screenW / 2 - promptWidth / 2 - 10, screenH / 2 + 20, promptWidth + 20, 28, Color{ 0, 0, 0, 160 });
                    DrawText(interactionPrompt.c_str(), screenW / 2 - promptWidth / 2, screenH / 2 + 26, promptFontSize, RAYWHITE);
                }
            }
            else
            {
                // Morrowind-style topic dialogue: the current line up top, a fixed set of
                // topics below it. There's no authored branching dialogue tree here (the
                // monologues file is just flat lines), so the topics are generic and shared
                // by every character - this demonstrates the interaction system, ready for
                // real per-character topics to be dropped in later.
                int boxWidth = 640;
                int fontSize = 16;
                int maxTextWidth = boxWidth - 32;

                std::vector<std::string> dialogueLines = WrapText(dialogueText, fontSize, maxTextWidth);
                const char* options[3] = {
                    "[1] Ask about the weather",
                    "[2] Ask who they are",
                    "[3] Farewell"
                };

                int textHeight = 40 + (int)dialogueLines.size() * 20;
                int boxHeight = textHeight + 3 * 22 + 16;
                int boxX = (screenW - boxWidth) / 2;
                int boxY = screenH - boxHeight - 70;
                DrawRectangle(boxX, boxY, boxWidth, boxHeight, Color{ 0, 0, 0, 200 });
                DrawRectangleLines(boxX, boxY, boxWidth, boxHeight, RAYWHITE);
                DrawText(dialogueName.c_str(), boxX + 16, boxY + 12, 18, RAYWHITE);
                for (size_t li = 0; li < dialogueLines.size(); li++)
                {
                    DrawText(dialogueLines[li].c_str(), boxX + 16, boxY + 40 + (int)li * 20, fontSize, LIGHTGRAY);
                }
                for (int oi = 0; oi < 3; oi++)
                {
                    DrawText(options[oi], boxX + 16, boxY + textHeight + oi * 22, fontSize, Color{ 210, 195, 150, 255 });
                }
            }
        EndDrawing();
    }

    UnloadModel(roadModel);
    UnloadModel(pavementModel);
    UnloadModel(fieldModel);
    // Detach before unloading so UnloadModel doesn't also try to free whichever painting
    // texture happened to be assigned last - those are unloaded separately just below.
    paintingModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{ 0 };
    UnloadModel(paintingModel);
    for (Texture2D& paintingTexture : paintingTextures) UnloadTexture(paintingTexture);
    UnloadTexture(cloudTexture);
    UnloadTexture(glowTexture);
    UnloadShader(fogShader);

    UnloadMusicStream(rainMusic);
    UnloadMusicStream(rainIndoorMusic);
    UnloadMusicStream(footstepsMusic);
    if (soundtrackMusicLoaded) UnloadMusicStream(soundtrackMusic);
    CloseAudioDevice();

    EnableCursor();
    CloseWindow();

    return 0;
}
