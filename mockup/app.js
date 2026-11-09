/* ==========================================================================
   usyseek — Desktop Application & Spotlight UI Mockup Controller
   ========================================================================== */

document.addEventListener('DOMContentLoaded', () => {

  // =========================================================================
  // 1. APPLICATION STATE
  // =========================================================================
  const state = {
    // Search & Filter
    searchQuery: '',
    similarityPath: null,
    activeFilter: 'all', // 'all' | 'images' | 'videos'
    activeTag: null,
    activeDir: null,
    isPillVisible: true,
    isFullscreen: false,
    pillWidth: 780,

    // Tagging System & Stamp Mode
    stampTag: null, // If set, clicking media applies/removes this tag
    selectedColorForNewTag: '#a8c7fa',
    selectedColorForDialog: '#a8c7fa',
    currentModalMedia: null,

    // Indexing & Progress
    isIndexing: true,
    embeddedCount: 14820,
    totalCount: 48200,
    indexingRate: 38, // files/sec
    currentScanningPath: 'C:\\Users\\usy\\Pictures\\Wallpapers\\neon_city_4k.png',
    scopeMode: 'all', // 'all' | 'custom'
    lastIndexedTimestamp: new Date(),

    // Directories
    includedDirs: [
      { path: 'C:\\Users\\usy\\Pictures', count: '24,310 files' },
      { path: 'D:\\Media\\Renderings', count: '12,180 files' },
      { path: 'C:\\Users\\usy\\Videos', count: '3,490 files' }
    ],
    excludedDirs: [
      { path: 'C:\\Windows', desc: 'System Root', system: true },
      { path: 'C:\\Program Files', desc: 'Applications', system: true },
      { path: 'C:\\Users\\usy\\AppData', desc: 'App Data & Cache', system: false },
      { path: '**\\node_modules\\**', desc: 'Pattern', system: false },
      { path: '**\\.git\\**', desc: 'Pattern', system: false },
      { path: 'C:\\Users\\usy\\Documents\\Private', desc: 'User Custom', system: false }
    ],

    // Global Keybind
    keybindEnabled: true,
    keybind: {
      altKey: true,
      ctrlKey: false,
      shiftKey: false,
      metaKey: false,
      key: ' ',
      display: ['Alt', 'Space']
    },
    isRecordingKeybind: false,

    // Active AI Model
    model: 'clip-vit-b32',
    modelSpecs: {
      'clip-vit-b32': { name: 'CLIP-ViT-B/32', dim: '512-d', vram: '~680 MB', latency: '12ms', device: 'CUDA (RTX 4070)' },
      'siglip-so400m': { name: 'SigLIP-SO400M', dim: '1152-d', vram: '~1.8 GB', latency: '28ms', device: 'CUDA (RTX 4070)' },
      'openclip-vit-l14': { name: 'OpenCLIP-ViT-L/14', dim: '768-d', vram: '~1.2 GB', latency: '18ms', device: 'CUDA (RTX 4070)' },
      'nomic-vision': { name: 'Nomic-Embed-Vision-v1.5', dim: '768-d', vram: '~950 MB', latency: '16ms', device: 'CUDA (RTX 4070)' },
      'custom': { name: 'Custom Local Path', dim: '768-d', vram: 'Variable', latency: '22ms', device: 'CUDA / Auto' }
    },

    // Venv Status
    venvActive: true
  };


  // =========================================================================
  // 2. MOCK MEDIA DATABASE (High Quality Visuals & Metadata)
  // =========================================================================
  const mediaDatabase = [
    {
      id: 'med-01',
      type: 'image',
      filename: 'cyberpunk_neon_alley_4k.png',
      path: 'C:\\Users\\usy\\Pictures\\Wallpapers\\cyberpunk_neon_alley_4k.png',
      tags: ['cyberpunk', 'neon', 'city', 'night', 'rain', 'futuristic', 'blue', 'pink', 'wallpaper'],
      resolution: '3840×2160',
      size: '14.2 MB',
      svgArt: 'cyberpunk',
      cluster: 'cyberpunk'
    },
    {
      id: 'med-02',
      type: 'video',
      filename: 'tokyo_night_drive_cinematic.mp4',
      path: 'C:\\Users\\usy\\Videos\\Clips\\tokyo_night_drive_cinematic.mp4',
      tags: ['video', 'tokyo', 'night', 'drive', 'cyberpunk', 'neon', 'rain', 'car', 'city'],
      resolution: '1920×1080',
      size: '84.6 MB',
      duration: '1:45',
      svgArt: 'cityvideo',
      cluster: 'cyberpunk'
    },
    {
      id: 'med-03',
      type: 'image',
      filename: 'sunset_pacific_highway_drive.jpg',
      path: 'C:\\Users\\usy\\Pictures\\Photography\\sunset_pacific_highway_drive.jpg',
      tags: ['sunset', 'beach', 'ocean', 'highway', 'california', 'orange', 'warm', 'photography'],
      resolution: '4096×2730',
      size: '9.8 MB',
      svgArt: 'sunset',
      cluster: 'sunset'
    },
    {
      id: 'med-04',
      type: 'image',
      filename: 'golden_hour_ocean_waves.png',
      path: 'C:\\Users\\usy\\Pictures\\Photography\\golden_hour_ocean_waves.png',
      tags: ['sunset', 'beach', 'ocean', 'waves', 'golden hour', 'water', 'nature'],
      resolution: '3840×2160',
      size: '12.1 MB',
      svgArt: 'waves',
      cluster: 'sunset'
    },
    {
      id: 'med-05',
      type: 'video',
      filename: 'drone_flyover_alpine_peaks.mp4',
      path: 'C:\\Users\\usy\\Videos\\Drone\\drone_flyover_alpine_peaks.mp4',
      tags: ['video', 'drone', 'mountains', 'snow', 'nature', 'landscape', 'alps', 'aerial'],
      resolution: '3840×2160 60fps',
      size: '240.5 MB',
      duration: '3:12',
      svgArt: 'mountains',
      cluster: 'nature'
    },
    {
      id: 'med-06',
      type: 'image',
      filename: 'misty_pine_forest_morning.jpg',
      path: 'C:\\Users\\usy\\Pictures\\Wallpapers\\misty_pine_forest_morning.jpg',
      tags: ['nature', 'forest', 'trees', 'mist', 'morning', 'fog', 'green', 'mountains'],
      resolution: '5120×2880',
      size: '16.7 MB',
      svgArt: 'forest',
      cluster: 'nature'
    },
    {
      id: 'med-07',
      type: 'image',
      filename: 'deep_learning_transformer_architecture.png',
      path: 'C:\\Users\\usy\\Documents\\Projects\\usyseek\\assets\\deep_learning_transformer_architecture.png',
      tags: ['diagram', 'chart', 'machine learning', 'transformer', 'neural network', 'ai', 'architecture'],
      resolution: '2560×1600',
      size: '3.4 MB',
      svgArt: 'diagram',
      cluster: 'tech'
    },
    {
      id: 'med-08',
      type: 'image',
      filename: 'anime_cozy_coffee_shop_rain.png',
      path: 'C:\\Users\\usy\\Pictures\\Art\\anime_cozy_coffee_shop_rain.png',
      tags: ['anime', 'art', 'cozy', 'coffee', 'cafe', 'rain', 'lofi', 'illustration', 'warm'],
      resolution: '3840×2160',
      size: '18.9 MB',
      svgArt: 'cozy',
      cluster: 'art'
    },
    {
      id: 'med-09',
      type: 'video',
      filename: 'shibuya_crossing_timelapse_4k.mp4',
      path: 'C:\\Users\\usy\\Videos\\Travel\\shibuya_crossing_timelapse_4k.mp4',
      tags: ['video', 'timelapse', 'tokyo', 'japan', 'city', 'crowd', 'street', 'night'],
      resolution: '3840×2160',
      size: '194.2 MB',
      duration: '0:58',
      svgArt: 'cityvideo',
      cluster: 'cyberpunk'
    },
    {
      id: 'med-10',
      type: 'image',
      filename: 'minimalist_nordic_interior_living.jpg',
      path: 'C:\\Users\\usy\\Pictures\\Design\\minimalist_nordic_interior_living.jpg',
      tags: ['interior', 'design', 'architecture', 'minimalist', 'living room', 'nordic', 'modern'],
      resolution: '4000×2667',
      size: '8.2 MB',
      svgArt: 'interior',
      cluster: 'design'
    },
    {
      id: 'med-11',
      type: 'image',
      filename: 'golden_retriever_running_snow.jpg',
      path: 'C:\\Users\\usy\\Pictures\\Pets\\golden_retriever_running_snow.jpg',
      tags: ['dog', 'pet', 'animal', 'snow', 'golden retriever', 'winter', 'cute', 'outdoor'],
      resolution: '3024×2016',
      size: '6.4 MB',
      svgArt: 'dog',
      cluster: 'pets'
    },
    {
      id: 'med-12',
      type: 'video',
      filename: 'aurora_borealis_norway_realtime.mp4',
      path: 'C:\\Users\\usy\\Videos\\Nature\\aurora_borealis_norway_realtime.mp4',
      tags: ['video', 'aurora', 'northern lights', 'norway', 'night', 'stars', 'green', 'sky'],
      resolution: '3840×2160',
      size: '310.8 MB',
      duration: '4:20',
      svgArt: 'aurora',
      cluster: 'nature'
    }
  ];


  // =========================================================================
  // 3. SVG ART GENERATOR (Crisp generative vectors for mock thumbnails)
  // =========================================================================
  function renderThumbnailArt(item) {
    const artType = item.svgArt || 'generic';
    let artMarkup = '';

    switch (artType) {
      case 'cyberpunk':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#141124"/>
            <rect x="10" y="45" width="40" height="155" fill="#1d1933"/>
            <rect x="95" y="30" width="55" height="170" fill="#262042"/>
            <rect x="195" y="50" width="45" height="150" fill="#1d1933"/>
            <rect x="40" y="70" width="45" height="130" fill="#2d244f"/>
            <rect x="140" y="65" width="50" height="135" fill="#35295c"/>
            <rect x="235" y="80" width="60" height="120" fill="#2d244f"/>
            <rect x="148" y="75" width="12" height="24" rx="2" fill="#f48fb1"/>
            <rect x="52" y="85" width="8" height="8" rx="1" fill="#78d9ec"/>
            <rect x="52" y="100" width="8" height="8" rx="1" fill="#ffb951"/>
            <rect x="250" y="95" width="16" height="6" rx="1" fill="#70d6b6"/>
            <rect x="250" y="108" width="16" height="6" rx="1" fill="#f48fb1"/>
            <circle cx="118" cy="45" r="4" fill="#f48fb1"/>
            <line x1="118" y1="49" x2="118" y2="70" stroke="#f48fb1" stroke-width="2"/>
            <rect x="0" y="170" width="320" height="30" fill="#0f0c1b"/>
            <line x1="20" y1="185" x2="60" y2="185" stroke="#35295c" stroke-width="3"/>
            <line x1="90" y1="185" x2="130" y2="185" stroke="#35295c" stroke-width="3"/>
            <line x1="160" y1="185" x2="200" y2="185" stroke="#35295c" stroke-width="3"/>
            <line x1="230" y1="185" x2="270" y2="185" stroke="#35295c" stroke-width="3"/>
          </svg>
        `;
        break;

      case 'cityvideo':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#101524"/>
            <circle cx="240" cy="50" r="22" fill="#ffb951"/>
            <circle cx="248" cy="46" r="19" fill="#101524"/>
            <polygon points="20,200 20,90 70,70 70,200" fill="#1b243b"/>
            <rect x="75" y="100" width="50" height="100" fill="#24304e"/>
            <rect x="135" y="80" width="60" height="120" fill="#1f2942"/>
            <rect x="205" y="110" width="45" height="90" fill="#283556"/>
            <rect x="260" y="95" width="50" height="105" fill="#1b243b"/>
            <path d="M0 160 Q 160 140 320 165 L320 200 L0 200 Z" fill="#0d111d"/>
            <path d="M0 168 Q 160 148 320 173" stroke="#ffb4ab" stroke-width="3" fill="none"/>
            <path d="M0 175 Q 160 155 320 180" stroke="#78d9ec" stroke-width="2.5" fill="none"/>
            <path d="M0 182 Q 160 162 320 187" stroke="#ffb951" stroke-width="2" fill="none"/>
          </svg>
        `;
        break;

      case 'sunset':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="55" y="0" fill="#4a154b"/>
            <rect width="320" height="40" y="55" fill="#a72626"/>
            <rect width="320" height="35" y="95" fill="#d35400"/>
            <rect width="320" height="25" y="130" fill="#f39c12"/>
            <circle cx="160" cy="115" r="32" fill="#ffffff"/>
            <rect width="320" height="45" y="155" fill="#1a3644"/>
            <rect x="120" y="163" width="80" height="2.5" rx="1" fill="#f1c40f"/>
            <rect x="100" y="171" width="120" height="2.5" rx="1" fill="#f39c12"/>
            <rect x="140" y="179" width="40" height="2.5" rx="1" fill="#e67e22"/>
          </svg>
        `;
        break;

      case 'waves':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="110" fill="#1b3a4b"/>
            <circle cx="210" cy="65" r="24" fill="#ffb951"/>
            <path d="M0 110 C 60 95, 100 120, 160 105 C 220 90, 260 115, 320 100 L320 200 L0 200 Z" fill="#2e6f7e"/>
            <path d="M0 135 C 70 120, 110 145, 180 130 C 240 115, 270 140, 320 125 L320 200 L0 200 Z" fill="#1f4e5b"/>
            <path d="M0 165 C 50 155, 90 175, 150 160 C 210 145, 270 170, 320 155 L320 200 L0 200 Z" fill="#13313d"/>
            <circle cx="160" cy="105" r="3.5" fill="#c2e7ff"/>
            <circle cx="180" cy="130" r="3.5" fill="#c2e7ff"/>
          </svg>
        `;
        break;

      case 'mountains':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#1b2838"/>
            <polygon points="80,200 160,40 240,200" fill="#2c3e50"/>
            <polygon points="160,40 180,75 140,75" fill="#eceff1"/>
            <polygon points="10,200 90,65 170,200" fill="#34495e"/>
            <polygon points="90,65 110,100 70,100" fill="#eceff1"/>
            <polygon points="170,200 250,55 330,200" fill="#34495e"/>
            <polygon points="250,55 270,90 230,90" fill="#eceff1"/>
            <polygon points="30,200 45,150 60,200" fill="#1e3a2f"/>
            <polygon points="55,200 70,140 85,200" fill="#193026"/>
            <polygon points="80,200 95,155 110,200" fill="#1e3a2f"/>
            <polygon points="240,200 255,145 270,200" fill="#193026"/>
            <polygon points="265,200 280,135 295,200" fill="#1e3a2f"/>
          </svg>
        `;
        break;

      case 'forest':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#162923"/>
            <polygon points="20,200 35,90 50,200" fill="#1f3b32"/>
            <polygon points="50,200 65,80 80,200" fill="#1f3b32"/>
            <polygon points="80,200 100,70 120,200" fill="#1f3b32"/>
            <polygon points="130,200 150,85 170,200" fill="#1f3b32"/>
            <polygon points="180,200 200,75 220,200" fill="#1f3b32"/>
            <polygon points="230,200 250,90 270,200" fill="#1f3b32"/>
            <polygon points="275,200 295,80 315,200" fill="#1f3b32"/>
            <polygon points="5,200 25,110 45,200" fill="#274d41"/>
            <polygon points="60,200 80,100 100,200" fill="#274d41"/>
            <polygon points="110,200 135,95 160,200" fill="#274d41"/>
            <polygon points="170,200 195,105 220,200" fill="#274d41"/>
            <polygon points="225,200 245,115 265,200" fill="#274d41"/>
            <polygon points="35,200 60,130 85,200" fill="#2f5e4f"/>
            <polygon points="140,200 170,120 200,200" fill="#2f5e4f"/>
            <polygon points="250,200 275,135 300,200" fill="#2f5e4f"/>
          </svg>
        `;
        break;

      case 'diagram':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#161b22"/>
            <line x1="0" y1="50" x2="320" y2="50" stroke="#21262d" stroke-width="1"/>
            <line x1="0" y1="100" x2="320" y2="100" stroke="#21262d" stroke-width="1"/>
            <line x1="0" y1="150" x2="320" y2="150" stroke="#21262d" stroke-width="1"/>
            <rect x="25" y="70" width="60" height="40" rx="6" fill="#1f293d" stroke="#58a6ff" stroke-width="2"/>
            <text x="55" y="94" fill="#a8c7fa" font-size="10" font-family="'JetBrains Mono', monospace" text-anchor="middle">Input</text>
            <line x1="85" y1="90" x2="125" y2="90" stroke="#a8c7fa" stroke-width="2"/>
            <rect x="125" y="55" width="85" height="70" rx="8" fill="#1e342a" stroke="#70d6b6" stroke-width="2"/>
            <text x="167" y="85" fill="#70d6b6" font-size="11" font-weight="bold" font-family="'Roboto', sans-serif" text-anchor="middle">Attention</text>
            <text x="167" y="102" fill="#c4eed0" font-size="9" font-family="'JetBrains Mono', monospace" text-anchor="middle">512-dim</text>
            <line x1="210" y1="90" x2="245" y2="90" stroke="#70d6b6" stroke-width="2"/>
            <rect x="245" y="70" width="55" height="40" rx="6" fill="#35263d" stroke="#d0bcff" stroke-width="2"/>
            <text x="272" y="94" fill="#d0bcff" font-size="10" font-family="'JetBrains Mono', monospace" text-anchor="middle">Output</text>
          </svg>
        `;
        break;

      case 'cozy':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#2d221c"/>
            <rect x="25" y="25" width="130" height="110" rx="4" fill="#1b232b" stroke="#48382f" stroke-width="3"/>
            <line x1="45" y1="40" x2="40" y2="55" stroke="#4a6572" stroke-width="1.5"/>
            <line x1="75" y1="60" x2="70" y2="75" stroke="#4a6572" stroke-width="1.5"/>
            <line x1="110" y1="45" x2="105" y2="60" stroke="#4a6572" stroke-width="1.5"/>
            <line x1="90" y1="80" x2="85" y2="95" stroke="#4a6572" stroke-width="1.5"/>
            <line x1="130" y1="70" x2="125" y2="85" stroke="#4a6572" stroke-width="1.5"/>
            <rect x="0" y="145" width="320" height="55" fill="#422f25"/>
            <rect x="195" y="115" width="40" height="45" rx="5" fill="#ffdcc2"/>
            <path d="M 235 125 C 248 125, 248 145, 235 145" fill="none" stroke="#ffdcc2" stroke-width="3.5"/>
            <path d="M 205 105 Q 200 95 205 85" fill="none" stroke="#ffddb9" stroke-width="2"/>
            <path d="M 215 105 Q 220 95 215 85" fill="none" stroke="#ffddb9" stroke-width="2"/>
          </svg>
        `;
        break;

      case 'interior':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="135" fill="#2a2e37"/>
            <rect x="0" y="135" width="320" height="65" fill="#1a1c22"/>
            <line x1="90" y1="0" x2="90" y2="45" stroke="#abb2bf" stroke-width="1.5"/>
            <polygon points="70,70 110,70 90,45" fill="#f8f9fa"/>
            <rect x="130" y="110" width="160" height="45" rx="6" fill="#3e4452"/>
            <rect x="138" y="90" width="144" height="25" rx="4" fill="#4b5263"/>
            <polygon points="50,150 45,180 65,180 60,150" fill="#e5c07b"/>
            <circle cx="55" cy="140" r="12" fill="#98c379"/>
            <circle cx="45" cy="130" r="10" fill="#70d6b6"/>
            <circle cx="65" cy="132" r="9" fill="#98c379"/>
          </svg>
        `;
        break;

      case 'dog':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="120" fill="#222c38"/>
            <circle cx="250" cy="50" r="26" fill="#ffd54f"/>
            <path d="M0 120 Q 120 100 220 125 T 320 110 L320 200 L0 200 Z" fill="#cfd8dc"/>
            <path d="M0 145 Q 160 130 320 150 L320 200 L0 200 Z" fill="#eceff1"/>
            <ellipse cx="145" cy="130" rx="24" ry="14" fill="#e67e22"/>
            <circle cx="168" cy="122" r="10" fill="#e67e22"/>
            <polygon points="166,116 172,110 174,116" fill="#d35400"/>
            <line x1="130" y1="135" x2="115" y2="148" stroke="#e67e22" stroke-width="4" stroke-linecap="round"/>
            <line x1="155" y1="138" x2="165" y2="152" stroke="#e67e22" stroke-width="4" stroke-linecap="round"/>
            <path d="M 122 128 Q 110 120 112 110" fill="none" stroke="#e67e22" stroke-width="3" stroke-linecap="round"/>
          </svg>
        `;
        break;

      case 'aurora':
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#0c1322"/>
            <circle cx="35" cy="30" r="1.5" fill="#fff"/>
            <circle cx="95" cy="45" r="1.5" fill="#fff"/>
            <circle cx="180" cy="25" r="1.5" fill="#fff"/>
            <circle cx="260" cy="40" r="1.5" fill="#fff"/>
            <path d="M 0 65 Q 80 30 160 65 T 320 50 L 320 90 Q 240 105 160 75 T 0 95 Z" fill="#00e676" opacity="0.65"/>
            <path d="M 0 50 Q 90 20 180 50 T 320 35 L 320 75 Q 230 90 140 60 T 0 75 Z" fill="#00e5ff" opacity="0.5"/>
            <polygon points="0,200 40,140 100,200" fill="#131e2e"/>
            <polygon points="80,200 160,120 240,200" fill="#17263a"/>
            <polygon points="200,200 270,135 320,180 320,200" fill="#131e2e"/>
          </svg>
        `;
        break;

      default:
        artMarkup = `
          <svg viewBox="0 0 320 200" width="100%" height="100%" preserveAspectRatio="xMidYMid slice">
            <rect width="320" height="200" fill="#1b212f"/>
            <circle cx="160" cy="100" r="38" fill="#263147"/>
            <polygon points="152,86 174,100 152,114" fill="#a8c7fa"/>
          </svg>
        `;
    }

    return artMarkup;
  }


  // =========================================================================
  // 4. UI DOM ELEMENTS SELECTION
  // =========================================================================
  const dom = {
    // Desktop & Mode
    windowsDesktop: document.getElementById('windowsDesktop'),
    btnToggleDesktopMode: document.getElementById('btnToggleDesktopMode'),
    desktopModeText: document.getElementById('desktopModeText'),
    btnResetLayout: document.getElementById('btnResetLayout'),
    desktopAppShortcut: document.getElementById('desktopAppShortcut'),

    // Spotlight Search Pill
    spotlightWrapper: document.getElementById('spotlightWrapper'),
    spotlightContainer: document.getElementById('spotlightContainer'),
    searchPillBar: document.getElementById('searchPillBar'),
    searchInput: document.getElementById('searchInput'),
    activeModeTag: document.getElementById('activeModeTag'),
    activeModeLabel: document.getElementById('activeModeLabel'),
    btnClearSimilarityTag: document.getElementById('btnClearSimilarityTag'),
    btnClearSearch: document.getElementById('btnClearSearch'),
    filterChips: document.querySelectorAll('.filter-pill-chip'),
    btnToggleTagRibbon: document.getElementById('btnToggleTagRibbon'),
    btnOpenSettings: document.getElementById('btnOpenSettings'),
    btnToggleFullscreen: document.getElementById('btnToggleFullscreen'),
    btnDismissPill: document.getElementById('btnDismissPill'),
    pillIndexingDot: document.getElementById('pillIndexingDot'),

    // Autocomplete Dropdown
    searchAutocomplete: document.getElementById('searchAutocomplete'),
    autocompleteList: document.getElementById('autocompleteList'),
    autocompleteHeader: document.getElementById('autocompleteHeader'),

    // Overall Tagging Menu Ribbon
    tagRibbonBar: document.getElementById('tagRibbonBar'),
    btnCloseTagRibbon: document.getElementById('btnCloseTagRibbon'),
    tagRibbonChips: document.getElementById('tagRibbonChips'),
    newTagInput: document.getElementById('newTagInput'),
    ribbonColorPalette: document.getElementById('ribbonColorPalette'),
    btnCreateNewTag: document.getElementById('btnCreateNewTag'),
    ribbonStampNotice: document.getElementById('ribbonStampNotice'),
    stampNoticeText: document.getElementById('stampNoticeText'),

    // Single Media Tag Editor Dialog Modal
    imageTagDialog: document.getElementById('imageTagDialog'),
    tagDialogTitlebar: document.getElementById('tagDialogTitlebar'),
    btnTagDialogClose: document.getElementById('btnTagDialogClose'),
    tagDialogThumb: document.getElementById('tagDialogThumb'),
    tagDialogFilename: document.getElementById('tagDialogFilename'),
    tagDialogPath: document.getElementById('tagDialogPath'),
    tagDialogSubinfo: document.getElementById('tagDialogSubinfo'),
    dialogAppliedTags: document.getElementById('dialogAppliedTags'),
    dialogTagInput: document.getElementById('dialogTagInput'),
    dialogColorPalette: document.getElementById('dialogColorPalette'),
    btnDialogAddTag: document.getElementById('btnDialogAddTag'),
    dialogAvailableTags: document.getElementById('dialogAvailableTags'),

    // Resizing
    handleLeft: document.getElementById('handleLeft'),
    handleRight: document.getElementById('handleRight'),
    resizeTooltip: document.getElementById('resizeTooltip'),

    // Results & Grid
    spotlightResultsPanel: document.getElementById('spotlightResultsPanel'),
    mediaResultsViewport: document.getElementById('mediaResultsViewport'),
    mediaGrid: document.getElementById('mediaGrid'),
    lastIndexedTime: document.getElementById('lastIndexedTime'),
    resultsLatency: document.getElementById('resultsLatency'),
    similarityActiveNotice: document.getElementById('similarityActiveNotice'),
    resultsEmptyState: document.getElementById('resultsEmptyState'),

    // Settings Window
    settingsWindow: document.getElementById('settingsWindow'),
    settingsTitlebar: document.getElementById('settingsTitlebar'),
    btnSettingsClose: document.getElementById('btnSettingsClose'),
    btnSettingsMinimize: document.getElementById('btnSettingsMinimize'),
    btnSettingsMaximize: document.getElementById('btnSettingsMaximize'),
    tabBtnProgress: document.getElementById('tabBtnProgress'),
    tabBtnSettings: document.getElementById('tabBtnSettings'),
    tabProgress: document.getElementById('tabProgress'),
    tabSettings: document.getElementById('tabSettings'),

    // Tab 1: Embeddings
    statEmbeddedCount: document.getElementById('statEmbeddedCount'),
    statTotalCount: document.getElementById('statTotalCount'),
    indexingStatusBadge: document.getElementById('indexingStatusBadge'),
    indexingStatusText: document.getElementById('indexingStatusText'),
    indexingRateText: document.getElementById('indexingRateText'),
    btnToggleIndexing: document.getElementById('btnToggleIndexing'),
    btnIndexingText: document.getElementById('btnIndexingText'),
    iconIndexingToggle: document.getElementById('iconIndexingToggle'),
    winProgressFill: document.getElementById('winProgressFill'),
    progressPercentageText: document.getElementById('progressPercentageText'),
    currentProcessingPath: document.getElementById('currentProcessingPath'),
    radioScopeAll: document.getElementById('radioScopeAll'),
    radioScopeCustom: document.getElementById('radioScopeCustom'),
    includedFoldersContainer: document.getElementById('includedFoldersContainer'),
    btnAddIncludedFolder: document.getElementById('btnAddIncludedFolder'),
    includedDirectoriesList: document.getElementById('includedDirectoriesList'),
    btnAddExcludedFolder: document.getElementById('btnAddExcludedFolder'),
    excludedDirectoriesList: document.getElementById('excludedDirectoriesList'),

    // Tab 2: Settings & Venv
    toggleKeybindEnabled: document.getElementById('toggleKeybindEnabled'),
    keybindStatusText: document.getElementById('keybindStatusText'),
    keyCap1: document.getElementById('keyCap1'),
    keyCap2: document.getElementById('keyCap2'),
    btnRecordKeybind: document.getElementById('btnRecordKeybind'),
    recordKeybindText: document.getElementById('recordKeybindText'),
    btnResetKeybind: document.getElementById('btnResetKeybind'),
    keybindHintMsg: document.getElementById('keybindHintMsg'),
    selectModel: document.getElementById('selectModel'),
    modelDim: document.getElementById('modelDim'),
    modelDevice: document.getElementById('modelDevice'),
    modelVram: document.getElementById('modelVram'),
    btnOpenTerminal: document.getElementById('btnOpenTerminal'),
    btnUpdateVenv: document.getElementById('btnUpdateVenv'),
    btnRecreateVenv: document.getElementById('btnRecreateVenv'),
    btnDeleteVenv: document.getElementById('btnDeleteVenv'),
    venvStatusBadge: document.getElementById('venvStatusBadge'),
    venvPathText: document.getElementById('venvPathText'),

    // Terminal
    terminalWindow: document.getElementById('terminalWindow'),
    terminalTitlebar: document.getElementById('terminalTitlebar'),
    btnTerminalClose: document.getElementById('btnTerminalClose'),
    terminalBody: document.getElementById('terminalBody'),
    termHistory: document.getElementById('termHistory'),
    termInput: document.getElementById('termInput'),

    // System Tray & Win32 Menu
    trayAppIcon: document.getElementById('trayAppIcon'),
    trayActiveDot: document.getElementById('trayActiveDot'),
    trayContextMenu: document.getElementById('trayContextMenu'),
    trayMenuStatus: document.getElementById('trayMenuStatus'),
    trayMenuOpenSearch: document.getElementById('trayMenuOpenSearch'),
    trayMenuOpenInterface: document.getElementById('trayMenuOpenInterface'),
    trayMenuToggleIndex: document.getElementById('trayMenuToggleIndex'),
    trayMenuIndexLabel: document.getElementById('trayMenuIndexLabel'),
    trayMenuQuit: document.getElementById('trayMenuQuit'),
    traySearchHotkey: document.getElementById('traySearchHotkey'),
    trayTime: document.getElementById('trayTime'),
    trayDate: document.getElementById('trayDate'),

    // Notifications & Hints
    toastContainer: document.getElementById('toastContainer'),
    topbarKeybindHint: document.getElementById('topbarKeybindHint')
  };


  // =========================================================================
  // 5. TOAST NOTIFICATION HELPER
  // =========================================================================
  function showToast(title, message, iconType = 'info') {
    const toast = document.createElement('div');
    toast.className = 'win-toast';
    
    let iconSvg = `
      <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" stroke-width="2">
        <circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/>
      </svg>
    `;

    if (iconType === 'success') {
      iconSvg = `
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="#70d6b6" stroke-width="2">
          <path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/>
        </svg>
      `;
    } else if (iconType === 'similar') {
      iconSvg = `
        <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="#a8c7fa" stroke-width="2">
          <circle cx="11" cy="11" r="7"/><path d="m21 21-4.35-4.35"/><path d="M11 8v6M8 11h6"/>
        </svg>
      `;
    }

    toast.innerHTML = `
      <div class="toast-icon">${iconSvg}</div>
      <div class="toast-content">
        <div class="toast-title">${title}</div>
        <div class="toast-message">${message}</div>
      </div>
    `;

    dom.toastContainer.appendChild(toast);

    setTimeout(() => {
      toast.classList.add('toast-out');
      setTimeout(() => toast.remove(), 300);
    }, 3800);
  }


  // =========================================================================
  // 5b. COLOR-COORDINATED TAGGING SYSTEM (Material 3 Tonal Palette)
  // =========================================================================
  const tagPaletteColors = [
    '#a8c7fa', // M3 Sky Blue
    '#70d6b6', // M3 Soft Mint
    '#ffb951', // M3 Warm Amber
    '#ffb4ab', // M3 Peach / Coral
    '#d0bcff', // M3 Lavender / Violet
    '#78d9ec', // M3 Cyan
    '#f48fb1', // M3 Rose Pink
    '#c4eed0'  // M3 Sage Green
  ];

  const globalTags = {
    'wallpaper': '#a8c7fa',
    'cyberpunk': '#f48fb1',
    'neon': '#78d9ec',
    'night': '#d0bcff',
    'sunset': '#ffb951',
    'ocean': '#78d9ec',
    'nature': '#70d6b6',
    'video': '#ffb4ab',
    'diagram': '#c4eed0',
    'architecture': '#a8c7fa',
    'anime': '#f48fb1',
    'cozy': '#ffb951',
    'dog': '#ffb4ab',
    'drone': '#78d9ec',
    'minimalist': '#c4eed0',
    'ai': '#a8c7fa'
  };

  function getTagColor(tagName) {
    const norm = tagName.toLowerCase();
    if (globalTags[norm]) return globalTags[norm];
    let hash = 0;
    for (let i = 0; i < norm.length; i++) {
      hash = norm.charCodeAt(i) + ((hash << 5) - hash);
    }
    const color = tagPaletteColors[Math.abs(hash) % tagPaletteColors.length];
    globalTags[norm] = color;
    return color;
  }

  function hexToRgba(hex, alpha = 0.2) {
    let c = hex.replace('#', '');
    if (c.length === 3) c = c.split('').map(x => x + x).join('');
    const num = parseInt(c, 16);
    const r = (num >> 16) & 255;
    const g = (num >> 8) & 255;
    const b = num & 255;
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }

  // Get all unique directories present in the media database
  function getMediaDirectories() {
    const dirs = new Map();
    mediaDatabase.forEach(item => {
      const parts = item.path.split('\\');
      parts.pop(); // remove filename
      const folderPath = parts.join('\\');
      dirs.set(folderPath, (dirs.get(folderPath) || 0) + 1);
    });
    return Array.from(dirs.entries()).map(([path, count]) => ({ path, count }));
  }


  // =========================================================================
  // 6. SEARCH & MEDIA RENDERING ENGINE
  // =========================================================================
  function formatLastIndexedTime() {
    if (state.isIndexing) {
      return 'Scanning live...';
    }
    const now = new Date();
    const diffSec = Math.floor((now - state.lastIndexedTimestamp) / 1000);
    if (diffSec < 60) return 'Just now';
    const diffMin = Math.floor(diffSec / 60);
    if (diffMin === 1) return '1 min ago';
    if (diffMin < 60) return `${diffMin} mins ago`;
    return now.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  function renderMediaResults() {
    const rawVal = state.searchQuery.trim();
    const query = rawVal.toLowerCase();

    const isSimilarityMode = query.startsWith('similar:');
    const isTagMode = query.startsWith('tag:');
    const isDirMode = query.startsWith('dir:') || 
                      query.startsWith('c:\\') || 
                      query.startsWith('d:\\') ||
                      query.includes('\\') ||
                      ['pictures', 'videos', 'wallpapers', 'clips', 'drone', 'art', 'design', 'pets', 'renderings', 'photography'].includes(query);

    let filtered = mediaDatabase.filter(item => {
      // Type filter
      if (state.activeFilter === 'images' && item.type !== 'image') return false;
      if (state.activeFilter === 'videos' && item.type !== 'video') return false;

      // 1. Tag Search (tag:<name>)
      if (isTagMode) {
        const targetTag = query.substring(4).trim();
        if (!targetTag) return true;
        return item.tags.some(t => t.toLowerCase().includes(targetTag));
      }

      // 2. Directory Search (dir:<path> or raw folder path)
      if (isDirMode) {
        let targetDir = query;
        if (targetDir.startsWith('dir:')) {
          targetDir = targetDir.substring(4).trim();
        }
        return item.path.toLowerCase().includes(targetDir);
      }

      // 3. Similarity Search (scored below)
      if (isSimilarityMode) {
        return true;
      }

      // 4. Standard semantic / filename / tag query
      if (query) {
        const matchesFilename = item.filename.toLowerCase().includes(query);
        const matchesTags = item.tags.some(t => t.toLowerCase().includes(query));
        const matchesPath = item.path.toLowerCase().includes(query);
        return matchesFilename || matchesTags || matchesPath;
      }

      return true;
    });

    // Similarity scoring calculation if in similarity mode
    if (isSimilarityMode) {
      const refItem = mediaDatabase.find(i => i.path.toLowerCase() === state.similarityPath.toLowerCase()) || mediaDatabase[0];
      const targetCluster = refItem ? refItem.cluster : 'cyberpunk';

      filtered = filtered.map(item => {
        let score = 0.55;
        if (item.path === refItem.path) {
          score = 0.998;
        } else if (item.cluster === targetCluster) {
          score = 0.88 + (Math.random() * 0.09);
        } else {
          score = 0.42 + (Math.random() * 0.35);
        }
        return { ...item, similarityScore: score };
      });

      // Sort by highest similarity
      filtered.sort((a, b) => b.similarityScore - a.similarityScore);
    }

    // Update Header Metadata (Last Indexed Time & Model Latency)
    if (dom.lastIndexedTime) {
      dom.lastIndexedTime.textContent = formatLastIndexedTime();
    }
    const spec = state.modelSpecs[state.model] || state.modelSpecs['clip-vit-b32'];
    dom.resultsLatency.textContent = `${spec.latency} (${spec.name})`;

    if (isSimilarityMode) {
      dom.similarityActiveNotice.style.display = 'inline-block';
      dom.similarityActiveNotice.textContent = 'Showing visual matches to reference media';
    } else if (isTagMode) {
      dom.similarityActiveNotice.style.display = 'inline-block';
      dom.similarityActiveNotice.textContent = `Filtered by tag: #${query.substring(4).trim()}`;
    } else if (isDirMode) {
      dom.similarityActiveNotice.style.display = 'inline-block';
      const cleanDir = query.startsWith('dir:') ? query.substring(4).trim() : query;
      dom.similarityActiveNotice.textContent = `Directory: ${cleanDir}`;
    } else {
      dom.similarityActiveNotice.style.display = 'none';
    }

    // Render Grid
    dom.mediaGrid.innerHTML = '';

    if (filtered.length === 0) {
      dom.resultsEmptyState.style.display = 'flex';
      return;
    } else {
      dom.resultsEmptyState.style.display = 'none';
    }

    filtered.forEach(item => {
      const card = document.createElement('div');
      card.className = `media-card ${state.stampTag ? 'stamp-cursor-mode' : ''}`;
      card.tabIndex = 0;
      card.setAttribute('data-id', item.id);
      card.setAttribute('data-path', item.path);

      const simPct = item.similarityScore ? (item.similarityScore * 100).toFixed(1) + '%' : null;

      // Render colored tag pills for this media (consistent single-line strip)
      const visibleTags = item.tags.slice(0, 2);
      const moreCount = item.tags.length > 2 ? item.tags.length - 2 : 0;
      const allTagsTooltip = item.tags.map(t => `#${t}`).join(', ');

      const tagChipsHtml = visibleTags.map(tag => {
        const col = getTagColor(tag);
        return `
          <span class="card-tag-pill" data-tag="${tag}" title="Filter by #${tag}" style="color: ${col}; background: ${hexToRgba(col, 0.14)}; border-color: ${hexToRgba(col, 0.3)};">
            <span class="tag-dot" style="background: ${col};"></span>${tag}
          </span>
        `;
      }).join('');

      card.innerHTML = `
        <div class="media-thumbnail-box">
          ${renderThumbnailArt(item)}
          
          ${item.type === 'video' ? `
            <span class="media-badge badge-video">
              <svg viewBox="0 0 24 24" width="10" height="10"><polygon points="5 3 19 12 5 21 5 3"/></svg>
              ${item.duration || '0:45'}
            </span>
          ` : ''}

          ${simPct ? `<span class="similarity-badge">${simPct} match</span>` : ''}

          <!-- Subtle little similarity icon in thumbnail corner (no large overlay!) -->
          <button class="btn-little-similar" title="Find visually similar media">
            <svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2.2">
              <circle cx="11" cy="11" r="7"/>
              <path d="m21 21-4.35-4.35"/>
              <path d="M11 8v6M8 11h6"/>
            </svg>
          </button>
        </div>

        <div class="media-card-body">
          <div class="media-filename" title="${item.filename}">${item.filename}</div>
          <div class="media-path" title="${item.path}">${item.path}</div>
          
          <!-- Colored Tag Pills (single-line strip, consistent across all media) -->
          <div class="card-tags-row" title="Tags: ${allTagsTooltip}">
            ${tagChipsHtml}
            ${moreCount > 0 ? `<span class="tag-pill-more" title="${allTagsTooltip}">+${moreCount}</span>` : ''}
          </div>

          <!-- Bottom-Pinned Metadata Footer (Always on the bottom of every card) -->
          <div class="media-meta-row">
            <span class="media-meta-specs">${item.resolution} • ${item.size}</span>
            <span class="card-tag-hint">${state.stampTag ? `Stamp #${state.stampTag}` : 'Click to tag'}</span>
          </div>
        </div>
      `;

      // Handle card interactions
      card.addEventListener('click', (e) => {
        // 1. If clicking a tag pill inside card, search by tag!
        const clickedTagPill = e.target.closest('.card-tag-pill');
        if (clickedTagPill) {
          e.stopPropagation();
          setTagSearch(clickedTagPill.getAttribute('data-tag'));
          return;
        }

        // 2. If clicking the subtle little similarity button in corner
        if (e.target.closest('.btn-little-similar')) {
          e.stopPropagation();
          setSimilaritySearch(item.path);
          return;
        }

        // 3. If in Stamp Mode from overall tagging menu: apply/remove tag with 1 click!
        if (state.stampTag) {
          e.stopPropagation();
          toggleTagOnItem(item, state.stampTag);
          return;
        }

        // 4. Normal mode: clicking image opens single-image tag editor modal
        openImageTagModal(item);
      });

      dom.mediaGrid.appendChild(card);
    });
  }

  // Set Search Input to "similar:<file path>"
  function setSimilaritySearch(filePath) {
    state.similarityPath = filePath;
    state.searchQuery = `similar:${filePath}`;
    
    dom.searchInput.value = state.searchQuery;
    dom.activeModeTag.style.display = 'inline-flex';
    dom.activeModeLabel.textContent = 'SIMILARITY';
    dom.btnClearSearch.style.display = 'flex';

    dom.searchInput.focus();
    hideAutocomplete();
    showToast('Similarity Search Activated', `Indexed matches for: ${filePath.split('\\').pop()}`, 'similar');
    renderMediaResults();
  }

  // Set Search Input to "tag:<tagName>"
  function setTagSearch(tagName) {
    state.similarityPath = null;
    state.searchQuery = `tag:${tagName}`;
    
    dom.searchInput.value = state.searchQuery;
    dom.activeModeTag.style.display = 'inline-flex';
    dom.activeModeLabel.textContent = `TAG: ${tagName}`;
    const col = getTagColor(tagName);
    dom.activeModeTag.style.borderColor = col;
    dom.activeModeTag.style.color = col;
    dom.btnClearSearch.style.display = 'flex';

    dom.searchInput.focus();
    hideAutocomplete();
    renderMediaResults();
  }

  // Set Search Input to "dir:<directory>"
  function setDirectorySearch(dirPath) {
    state.similarityPath = null;
    state.searchQuery = `dir:${dirPath}`;
    
    dom.searchInput.value = state.searchQuery;
    dom.activeModeTag.style.display = 'inline-flex';
    dom.activeModeLabel.textContent = `DIR: ${dirPath.split('\\').pop() || dirPath}`;
    dom.activeModeTag.style.borderColor = '#a8c7fa';
    dom.activeModeTag.style.color = '#a8c7fa';
    dom.btnClearSearch.style.display = 'flex';

    dom.searchInput.focus();
    hideAutocomplete();
    renderMediaResults();
  }

  // Clear Active Search
  function clearActiveSearch() {
    state.similarityPath = null;
    state.searchQuery = '';
    dom.searchInput.value = '';
    dom.activeModeTag.style.display = 'none';
    dom.btnClearSearch.style.display = 'none';
    hideAutocomplete();
    renderMediaResults();
    dom.searchInput.focus();
  }


  // =========================================================================
  // 7. INPUT LISTENERS, AUTOCOMPLETE & DIRECTORY PARSER
  // =========================================================================
  dom.searchInput.addEventListener('input', (e) => {
    const val = e.target.value;
    state.searchQuery = val;
    const lower = val.toLowerCase().trim();

    if (val.length > 0) {
      dom.btnClearSearch.style.display = 'flex';
    } else {
      dom.btnClearSearch.style.display = 'none';
    }

    // 1. Tag mode detection & autocomplete
    if (lower.startsWith('tag:')) {
      const tagPrefix = lower.substring(4).trim();
      dom.activeModeTag.style.display = 'inline-flex';
      dom.activeModeLabel.textContent = `TAG: ${tagPrefix || '...'}`;
      showTagAutocomplete(tagPrefix);
      renderMediaResults();
      return;
    }

    // 2. Directory mode detection & autocomplete
    if (lower.startsWith('dir:') || lower.startsWith('c:\\') || lower.startsWith('d:\\') || lower.includes('\\')) {
      let dirTerm = lower.startsWith('dir:') ? lower.substring(4).trim() : lower;
      dom.activeModeTag.style.display = 'inline-flex';
      dom.activeModeLabel.textContent = 'DIRECTORY';
      showDirectoryAutocomplete(dirTerm);
      renderMediaResults();
      return;
    }

    // 3. Similarity mode detection
    if (lower.startsWith('similar:')) {
      state.similarityPath = val.substring(8).trim();
      dom.activeModeTag.style.display = 'inline-flex';
      dom.activeModeLabel.textContent = 'SIMILARITY';
      hideAutocomplete();
      renderMediaResults();
      return;
    }

    // 4. General search: check if user typed a keyword that matches directories or tags
    dom.activeModeTag.style.display = 'none';
    state.similarityPath = null;

    if (lower.length >= 2) {
      // Suggest tags or folders if typing matches
      const matchingTags = Object.keys(globalTags).filter(t => t.includes(lower));
      const matchingDirs = getMediaDirectories().filter(d => d.path.toLowerCase().includes(lower));
      if (matchingTags.length > 0 || matchingDirs.length > 0) {
        showGeneralAutocomplete(lower, matchingTags, matchingDirs);
      } else {
        hideAutocomplete();
      }
    } else {
      hideAutocomplete();
    }

    renderMediaResults();
  });

  dom.btnClearSearch.addEventListener('click', () => {
    clearActiveSearch();
  });

  dom.btnClearSimilarityTag.addEventListener('click', () => {
    clearActiveSearch();
  });

  // Autocomplete helpers
  function showTagAutocomplete(filterTerm) {
    dom.autocompleteHeader.textContent = 'MATCHING TAGS';
    dom.autocompleteList.innerHTML = '';

    const allTags = Object.keys(globalTags);
    const matches = allTags.filter(t => !filterTerm || t.includes(filterTerm));

    if (matches.length === 0) {
      dom.autocompleteList.innerHTML = `
        <div class="autocomplete-item" style="color: var(--text-tertiary);">
          <span>No tags match "tag:${filterTerm}"</span>
        </div>
      `;
    } else {
      matches.forEach(tag => {
        const col = getTagColor(tag);
        const count = mediaDatabase.filter(m => m.tags.includes(tag)).length;
        const item = document.createElement('div');
        item.className = 'autocomplete-item';
        item.innerHTML = `
          <div class="autocomplete-item-left">
            <span class="tag-dot" style="background: ${col}; width: 8px; height: 8px;"></span>
            <span>tag:<strong>${tag}</strong></span>
          </div>
          <span class="autocomplete-item-count">${count} items</span>
        `;
        item.addEventListener('click', () => {
          setTagSearch(tag);
        });
        dom.autocompleteList.appendChild(item);
      });
    }

    dom.searchAutocomplete.style.display = 'block';
  }

  function showDirectoryAutocomplete(filterTerm) {
    dom.autocompleteHeader.textContent = 'MEDIA DIRECTORIES';
    dom.autocompleteList.innerHTML = '';

    const dirs = getMediaDirectories();
    const matches = dirs.filter(d => !filterTerm || d.path.toLowerCase().includes(filterTerm));

    if (matches.length === 0) {
      dom.autocompleteList.innerHTML = `
        <div class="autocomplete-item" style="color: var(--text-tertiary);">
          <span>No directories match "${filterTerm}"</span>
        </div>
      `;
    } else {
      matches.forEach(dir => {
        const item = document.createElement('div');
        item.className = 'autocomplete-item';
        item.innerHTML = `
          <div class="autocomplete-item-left">
            <span>📁</span>
            <span>${dir.path}</span>
          </div>
          <span class="autocomplete-item-count">${dir.count} files</span>
        `;
        item.addEventListener('click', () => {
          setDirectorySearch(dir.path);
        });
        dom.autocompleteList.appendChild(item);
      });
    }

    dom.searchAutocomplete.style.display = 'block';
  }

  function showGeneralAutocomplete(term, tags, dirs) {
    dom.autocompleteHeader.textContent = 'SUGGESTED TAGS & FOLDERS';
    dom.autocompleteList.innerHTML = '';

    tags.slice(0, 4).forEach(tag => {
      const col = getTagColor(tag);
      const item = document.createElement('div');
      item.className = 'autocomplete-item';
      item.innerHTML = `
        <div class="autocomplete-item-left">
          <span class="tag-dot" style="background: ${col}; width: 8px; height: 8px;"></span>
          <span>tag:<strong>${tag}</strong></span>
        </div>
        <span class="autocomplete-item-count">Tag filter</span>
      `;
      item.addEventListener('click', () => setTagSearch(tag));
      dom.autocompleteList.appendChild(item);
    });

    dirs.slice(0, 3).forEach(dir => {
      const item = document.createElement('div');
      item.className = 'autocomplete-item';
      item.innerHTML = `
        <div class="autocomplete-item-left">
          <span>📁</span>
          <span>${dir.path}</span>
        </div>
        <span class="autocomplete-item-count">${dir.count} files</span>
      `;
      item.addEventListener('click', () => setDirectorySearch(dir.path));
      dom.autocompleteList.appendChild(item);
    });

    dom.searchAutocomplete.style.display = 'block';
  }

  function hideAutocomplete() {
    dom.searchAutocomplete.style.display = 'none';
  }

  // Close autocomplete on click outside
  window.addEventListener('click', (e) => {
    if (!e.target.closest('#searchAutocomplete') && !e.target.closest('#searchInput')) {
      hideAutocomplete();
    }
  });


  // =========================================================================
  // 7b. OVERALL TAGGING MENU RIBBON (Stamp Mode & Tag Management)
  // =========================================================================
  dom.btnToggleTagRibbon.addEventListener('click', () => {
    const isVisible = dom.tagRibbonBar.style.display !== 'none';
    if (isVisible) {
      dom.tagRibbonBar.style.display = 'none';
      state.stampTag = null;
      renderRibbonChips();
      renderMediaResults();
    } else {
      dom.tagRibbonBar.style.display = 'flex';
      renderRibbonColorPalette();
      renderRibbonChips();
    }
  });

  dom.btnCloseTagRibbon.addEventListener('click', () => {
    dom.tagRibbonBar.style.display = 'none';
    state.stampTag = null;
    renderMediaResults();
  });

  function renderRibbonChips() {
    dom.tagRibbonChips.innerHTML = '';
    const allTags = Object.keys(globalTags);

    allTags.forEach(tag => {
      const col = getTagColor(tag);
      const count = mediaDatabase.filter(m => m.tags.includes(tag)).length;
      const isStamp = state.stampTag === tag;

      const btn = document.createElement('button');
      btn.className = `ribbon-tag-chip ${isStamp ? 'active-stamp' : ''}`;
      btn.style.color = col;
      btn.style.backgroundColor = isStamp ? col : hexToRgba(col, 0.16);
      if (isStamp) btn.style.color = '#141218';
      btn.innerHTML = `
        <span class="tag-dot" style="background: ${isStamp ? '#141218' : col};"></span>
        ${tag} (${count})
      `;

      btn.addEventListener('click', () => {
        if (state.stampTag === tag) {
          state.stampTag = null;
          dom.ribbonStampNotice.style.display = 'none';
          showToast('Stamp Mode Disabled', 'Clicking images will now open tag details');
        } else {
          state.stampTag = tag;
          dom.ribbonStampNotice.style.display = 'inline-flex';
          dom.stampNoticeText.textContent = `Stamp Mode: #${tag}`;
          showToast(`Stamp Mode: #${tag}`, `Click any media card below to add or remove #${tag}`, 'success');
        }
        renderRibbonChips();
        renderMediaResults();
      });

      dom.tagRibbonChips.appendChild(btn);
    });
  }

  function renderRibbonColorPalette() {
    dom.ribbonColorPalette.innerHTML = '';
    tagPaletteColors.forEach(color => {
      const dot = document.createElement('div');
      dot.className = `color-swatch-dot ${state.selectedColorForNewTag === color ? 'active' : ''}`;
      dot.style.backgroundColor = color;
      dot.addEventListener('click', () => {
        state.selectedColorForNewTag = color;
        renderRibbonColorPalette();
      });
      dom.ribbonColorPalette.appendChild(dot);
    });
  }

  dom.btnCreateNewTag.addEventListener('click', createNewTagFromRibbon);
  dom.newTagInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') createNewTagFromRibbon();
  });

  function createNewTagFromRibbon() {
    const raw = dom.newTagInput.value.trim().toLowerCase().replace(/[^a-z0-9_-]/g, '');
    if (!raw) return;

    globalTags[raw] = state.selectedColorForNewTag;
    dom.newTagInput.value = '';
    renderRibbonChips();
    showToast('New Tag Created', `Created color-coordinated tag #${raw}`, 'success');
  }

  // Toggle tag on media item (Stamp mode)
  function toggleTagOnItem(item, tagName) {
    const idx = item.tags.indexOf(tagName);
    if (idx !== -1) {
      item.tags.splice(idx, 1);
      showToast('Tag Removed', `Removed #${tagName} from ${item.filename}`);
    } else {
      item.tags.push(tagName);
      showToast('Tag Added', `Added #${tagName} to ${item.filename}`, 'success');
    }
    renderRibbonChips();
    renderMediaResults();
  }


  // =========================================================================
  // 7c. SINGLE MEDIA TAG EDITOR DIALOG MODAL
  // =========================================================================
  function openImageTagModal(item) {
    state.currentModalMedia = item;
    dom.tagDialogFilename.textContent = item.filename;
    dom.tagDialogPath.textContent = item.path;
    dom.tagDialogSubinfo.textContent = `${item.resolution} • ${item.size} • ${item.type.toUpperCase()}`;
    dom.tagDialogThumb.innerHTML = renderThumbnailArt(item);

    renderDialogAppliedTags();
    renderDialogColorPalette();
    renderDialogAvailableTags();

    dom.imageTagDialog.style.display = 'block';
  }

  dom.btnTagDialogClose.addEventListener('click', () => {
    dom.imageTagDialog.style.display = 'none';
    state.currentModalMedia = null;
  });

  function renderDialogAppliedTags() {
    if (!state.currentModalMedia) return;
    dom.dialogAppliedTags.innerHTML = '';

    state.currentModalMedia.tags.forEach(tag => {
      const col = getTagColor(tag);
      const badge = document.createElement('span');
      badge.className = 'dialog-tag-badge';
      badge.style.color = col;
      badge.style.backgroundColor = hexToRgba(col, 0.18);
      badge.style.borderColor = hexToRgba(col, 0.4);
      badge.innerHTML = `
        <span class="tag-dot" style="background: ${col};"></span>
        ${tag}
        <button class="btn-remove-tag" title="Remove tag">✕</button>
      `;

      badge.querySelector('.btn-remove-tag').addEventListener('click', () => {
        state.currentModalMedia.tags = state.currentModalMedia.tags.filter(t => t !== tag);
        renderDialogAppliedTags();
        renderDialogAvailableTags();
        renderMediaResults();
        renderRibbonChips();
        showToast('Tag Removed', `Removed #${tag}`);
      });

      dom.dialogAppliedTags.appendChild(badge);
    });
  }

  function renderDialogColorPalette() {
    dom.dialogColorPalette.innerHTML = '';
    tagPaletteColors.forEach(color => {
      const dot = document.createElement('div');
      dot.className = `color-swatch-dot ${state.selectedColorForDialog === color ? 'active' : ''}`;
      dot.style.backgroundColor = color;
      dot.addEventListener('click', () => {
        state.selectedColorForDialog = color;
        renderDialogColorPalette();
      });
      dom.dialogColorPalette.appendChild(dot);
    });
  }

  function renderDialogAvailableTags() {
    if (!state.currentModalMedia) return;
    dom.dialogAvailableTags.innerHTML = '';

    const allTags = Object.keys(globalTags);
    const unapplied = allTags.filter(t => !state.currentModalMedia.tags.includes(t));

    unapplied.forEach(tag => {
      const col = getTagColor(tag);
      const chip = document.createElement('button');
      chip.className = 'suggest-tag-chip';
      chip.innerHTML = `+ ${tag}`;
      chip.style.borderColor = hexToRgba(col, 0.3);
      chip.addEventListener('click', () => {
        state.currentModalMedia.tags.push(tag);
        renderDialogAppliedTags();
        renderDialogAvailableTags();
        renderMediaResults();
        renderRibbonChips();
        showToast('Tag Added', `Added #${tag} to ${state.currentModalMedia.filename}`, 'success');
      });
      dom.dialogAvailableTags.appendChild(chip);
    });
  }

  dom.btnDialogAddTag.addEventListener('click', addTagFromDialog);
  dom.dialogTagInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') addTagFromDialog();
  });

  function addTagFromDialog() {
    if (!state.currentModalMedia) return;
    const raw = dom.dialogTagInput.value.trim().toLowerCase().replace(/[^a-z0-9_-]/g, '');
    if (!raw) return;

    // Set custom chosen color for new tag
    globalTags[raw] = state.selectedColorForDialog;

    if (!state.currentModalMedia.tags.includes(raw)) {
      state.currentModalMedia.tags.push(raw);
    }

    dom.dialogTagInput.value = '';
    renderDialogAppliedTags();
    renderDialogAvailableTags();
    renderMediaResults();
    renderRibbonChips();
    showToast('Tag Saved', `Added #${raw} with custom color`, 'success');
  }

  // Category filter chips (All, Images, Videos)
  dom.filterChips.forEach(chip => {
    chip.addEventListener('click', () => {
      dom.filterChips.forEach(c => c.classList.remove('active'));
      chip.classList.add('active');
      state.activeFilter = chip.getAttribute('data-filter');
      renderMediaResults();
    });
  });


  // =========================================================================
  // 8. PILL DRAGGING & RESIZING INTERACTION
  // =========================================================================
  let isResizing = false;
  let activeHandle = null;
  let startX = 0;
  let startWidth = 780;

  // Draggable pill state (allows moving search pill anywhere on screen)
  let isDraggingPill = false;
  let pillDragStartX = 0;
  let pillDragStartY = 0;
  let pillInitialLeft = 0;
  let pillInitialTop = 0;
  let savedPillPosition = null;

  dom.searchPillBar.addEventListener('mousedown', (e) => {
    // Ignore clicks on input, buttons, filter chips, or resize handles
    if (e.target.closest('input, button, .filter-pill-chip, .resize-handle')) {
      return;
    }
    if (state.isFullscreen) return;

    isDraggingPill = true;
    dom.spotlightWrapper.classList.add('is-dragging');

    const rect = dom.spotlightWrapper.getBoundingClientRect();
    // Switch from centered transform to explicit pixel coordinates on first drag
    dom.spotlightWrapper.style.transform = 'none';
    dom.spotlightWrapper.style.left = `${rect.left}px`;
    dom.spotlightWrapper.style.top = `${rect.top}px`;

    pillDragStartX = e.clientX;
    pillDragStartY = e.clientY;
    pillInitialLeft = rect.left;
    pillInitialTop = rect.top;

    e.preventDefault();
  });

  function initResize(handle, e) {
    if (state.isFullscreen) return;
    isResizing = true;
    activeHandle = handle;
    startX = e.clientX;
    startWidth = dom.spotlightWrapper.offsetWidth;
    handle.classList.add('active');
    dom.resizeTooltip.classList.add('visible');
    dom.resizeTooltip.textContent = `${startWidth}px`;
    document.body.style.cursor = 'ew-resize';
    e.preventDefault();
  }

  dom.handleLeft.addEventListener('mousedown', (e) => initResize(dom.handleLeft, e));
  dom.handleRight.addEventListener('mousedown', (e) => initResize(dom.handleRight, e));

  // Double click resets to standard 780px
  dom.handleLeft.addEventListener('dblclick', () => resetPillWidth());
  dom.handleRight.addEventListener('dblclick', () => resetPillWidth());

  function resetPillWidth() {
    state.pillWidth = 780;
    dom.spotlightWrapper.style.width = '780px';
    dom.resizeTooltip.textContent = '780px';
    showToast('Layout Reset', 'Search pill width reset to 780px');
  }

  window.addEventListener('mousemove', (e) => {
    // 1. Move Search Pill
    if (isDraggingPill) {
      const dx = e.clientX - pillDragStartX;
      const dy = e.clientY - pillDragStartY;

      const wrapperWidth = dom.spotlightWrapper.offsetWidth;
      let nextLeft = pillInitialLeft + dx;
      let nextTop = pillInitialTop + dy;

      // Keep within desktop viewport bounds
      const maxLeft = window.innerWidth - wrapperWidth - 10;
      const maxTop = window.innerHeight - 80;

      nextLeft = Math.max(10, Math.min(nextLeft, maxLeft));
      nextTop = Math.max(10, Math.min(nextTop, maxTop));

      dom.spotlightWrapper.style.left = `${nextLeft}px`;
      dom.spotlightWrapper.style.top = `${nextTop}px`;
      savedPillPosition = { left: nextLeft, top: nextTop };
      return;
    }

    // 2. Resize Search Pill
    if (!isResizing) return;

    const deltaX = e.clientX - startX;
    let newWidth = startWidth;

    if (activeHandle === dom.handleRight) {
      newWidth = startWidth + (deltaX * 2); // multiply by 2 because centered
    } else if (activeHandle === dom.handleLeft) {
      newWidth = startWidth - (deltaX * 2);
    }

    // Clamp width: 520px to (window.innerWidth - 60px)
    const maxWidth = Math.min(1440, window.innerWidth - 60);
    newWidth = Math.max(520, Math.min(newWidth, maxWidth));

    state.pillWidth = newWidth;
    dom.spotlightWrapper.style.width = `${newWidth}px`;
    dom.resizeTooltip.textContent = `${Math.round(newWidth)}px`;
  });

  window.addEventListener('mouseup', () => {
    if (isDraggingPill) {
      isDraggingPill = false;
      dom.spotlightWrapper.classList.remove('is-dragging');
    }
    if (isResizing) {
      isResizing = false;
      if (activeHandle) activeHandle.classList.remove('active');
      activeHandle = null;
      dom.resizeTooltip.classList.remove('visible');
      document.body.style.cursor = '';
    }
  });


  // =========================================================================
  // 9. FULLSCREEN SEARCH TOGGLE & PILL CONTROLS
  // =========================================================================
  dom.btnToggleFullscreen.addEventListener('click', toggleFullscreen);

  function toggleFullscreen() {
    state.isFullscreen = !state.isFullscreen;
    const iconExpand = dom.btnToggleFullscreen.querySelector('.icon-expand');
    const iconCompress = dom.btnToggleFullscreen.querySelector('.icon-compress');

    if (state.isFullscreen) {
      // Save current position before going fullscreen
      if (!savedPillPosition) {
        const rect = dom.spotlightWrapper.getBoundingClientRect();
        savedPillPosition = { left: rect.left, top: rect.top };
      }
      dom.spotlightWrapper.classList.add('is-fullscreen');
      iconExpand.style.display = 'none';
      iconCompress.style.display = 'block';
      dom.btnToggleFullscreen.title = 'Exit Fullscreen (Esc)';
    } else {
      dom.spotlightWrapper.classList.remove('is-fullscreen');
      dom.spotlightWrapper.style.width = `${state.pillWidth}px`;
      if (savedPillPosition) {
        dom.spotlightWrapper.style.left = `${savedPillPosition.left}px`;
        dom.spotlightWrapper.style.top = `${savedPillPosition.top}px`;
        dom.spotlightWrapper.style.transform = 'none';
      } else {
        dom.spotlightWrapper.style.left = '50%';
        dom.spotlightWrapper.style.top = '12%';
        dom.spotlightWrapper.style.transform = 'translateX(-50%)';
      }
      iconExpand.style.display = 'block';
      iconCompress.style.display = 'none';
      dom.btnToggleFullscreen.title = 'Fullscreen search interface';
    }
    renderMediaResults();
  }

  // Pill Visibility (Dismiss & Show)
  dom.btnDismissPill.addEventListener('click', () => {
    hidePill();
  });

  function hidePill() {
    dom.spotlightWrapper.classList.add('hidden');
    state.isPillVisible = false;
  }

  function showPill() {
    dom.spotlightWrapper.classList.remove('hidden');
    state.isPillVisible = true;
    setTimeout(() => dom.searchInput.focus(), 120);
  }

  function togglePill() {
    if (state.isPillVisible) {
      hidePill();
    } else {
      showPill();
    }
  }


  // =========================================================================
  // 10. SETTINGS & EMBEDDINGS PROGRESS WINDOW
  // =========================================================================
  dom.btnOpenSettings.addEventListener('click', () => {
    openSettingsWindow();
  });

  dom.desktopAppShortcut.addEventListener('dblclick', () => {
    showPill();
  });

  dom.btnSettingsClose.addEventListener('click', () => {
    dom.settingsWindow.style.display = 'none';
  });

  dom.btnSettingsMinimize.addEventListener('click', () => {
    dom.settingsWindow.style.display = 'none';
    showToast('usyseek Minimized', 'Settings minimized to system tray');
  });

  let isSettingsMaximized = false;
  dom.btnSettingsMaximize.addEventListener('click', () => {
    isSettingsMaximized = !isSettingsMaximized;
    if (isSettingsMaximized) {
      dom.settingsWindow.style.top = '10px';
      dom.settingsWindow.style.left = '10px';
      dom.settingsWindow.style.width = 'calc(100vw - 20px)';
      dom.settingsWindow.style.height = 'calc(100vh - 60px)';
    } else {
      dom.settingsWindow.style.top = '14%';
      dom.settingsWindow.style.left = '25%';
      dom.settingsWindow.style.width = '740px';
      dom.settingsWindow.style.height = 'auto';
    }
  });

  function openSettingsWindow(tabName = 'tabProgress') {
    dom.settingsWindow.style.display = 'flex';
    switchTab(tabName);
  }

  // Window Tab Switching
  dom.tabBtnProgress.addEventListener('click', () => switchTab('tabProgress'));
  dom.tabBtnSettings.addEventListener('click', () => switchTab('tabSettings'));

  function switchTab(tabId) {
    if (tabId === 'tabProgress') {
      dom.tabBtnProgress.classList.add('active');
      dom.tabBtnSettings.classList.remove('active');
      dom.tabProgress.style.display = 'flex';
      dom.tabSettings.style.display = 'none';
    } else {
      dom.tabBtnProgress.classList.remove('active');
      dom.tabBtnSettings.classList.add('active');
      dom.tabProgress.style.display = 'none';
      dom.tabSettings.style.display = 'flex';
    }
  }

  // Make Windows Draggable by Titlebar
  function makeDraggable(windowEl, titlebarEl) {
    if (!windowEl || !titlebarEl) return;
    let isDragging = false;
    let startX = 0, startY = 0, initLeft = 0, initTop = 0;

    titlebarEl.addEventListener('mousedown', (e) => {
      if (e.target.closest('.win-window-controls')) return;
      isDragging = true;
      startX = e.clientX;
      startY = e.clientY;
      const rect = windowEl.getBoundingClientRect();
      initLeft = rect.left;
      initTop = rect.top;
      windowEl.style.transform = 'none';
      e.preventDefault();
    });

    window.addEventListener('mousemove', (e) => {
      if (!isDragging) return;
      const dx = e.clientX - startX;
      const dy = e.clientY - startY;
      windowEl.style.left = `${Math.max(0, initLeft + dx)}px`;
      windowEl.style.top = `${Math.max(0, initTop + dy)}px`;
    });

    window.addEventListener('mouseup', () => {
      isDragging = false;
    });
  }

  makeDraggable(dom.settingsWindow, dom.settingsTitlebar);
  makeDraggable(dom.terminalWindow, dom.terminalTitlebar);
  makeDraggable(dom.imageTagDialog, dom.tagDialogTitlebar);


  // =========================================================================
  // 11. EMBEDDINGS PROGRESS & SCOPE DIRECTORIES
  // =========================================================================
  // Start / Stop Indexing Toggle
  dom.btnToggleIndexing.addEventListener('click', toggleIndexing);

  function toggleIndexing() {
    state.isIndexing = !state.isIndexing;
    updateIndexingUI();
  }

  function updateIndexingUI() {
    state.lastIndexedTimestamp = new Date();
    if (dom.lastIndexedTime) dom.lastIndexedTime.textContent = formatLastIndexedTime();

    if (state.isIndexing) {
      dom.btnIndexingText.textContent = 'Stop Indexing';
      dom.iconIndexingToggle.innerHTML = `
        <rect x="6" y="4" width="4" height="16" rx="1"/>
        <rect x="14" y="4" width="4" height="16" rx="1"/>
      `;
      dom.indexingStatusBadge.className = 'status-badge';
      dom.indexingStatusText.textContent = 'Indexing Active';
      dom.indexingRateText.textContent = `~${state.indexingRate} files/sec • Live background worker`;
      
      dom.pillIndexingDot.classList.add('indexing');
      dom.trayActiveDot.classList.add('indexing');
      dom.trayMenuIndexLabel.textContent = 'Stop Index';
      dom.trayMenuStatus.textContent = `Indexing (${state.embeddedCount.toLocaleString()} files)`;
    } else {
      dom.btnIndexingText.textContent = 'Start Indexing';
      dom.iconIndexingToggle.innerHTML = `<polygon points="5 3 19 12 5 21 5 3"/>`;
      dom.indexingStatusBadge.className = 'status-badge status-badge-ok';
      dom.indexingStatusText.textContent = 'Paused / Idle';
      dom.indexingRateText.textContent = 'Worker idle (0 files/sec)';
      
      dom.pillIndexingDot.classList.remove('indexing');
      dom.trayActiveDot.classList.remove('indexing');
      dom.trayMenuIndexLabel.textContent = 'Start Index';
      dom.trayMenuStatus.textContent = `Idle (${state.embeddedCount.toLocaleString()} files)`;
    }
  }

  // Live Simulated Indexing Counter
  const sampleIndexFiles = [
    'C:\\Users\\usy\\Pictures\\Holiday2025\\DSC_0982.RAW',
    'C:\\Users\\usy\\Pictures\\Wallpapers\\cyberpunk_city_8k.png',
    'D:\\Media\\Clips\\gopro_mountain_biking.mp4',
    'C:\\Users\\usy\\Pictures\\Screenshots\\architecture_spec.png',
    'C:\\Users\\usy\\Videos\\Travel\\kyoto_bamboo_forest.mov',
    'D:\\Media\\Renderings\\blender_character_mesh.png'
  ];

  // Live Simulated Indexing Counter (Smooth, non-jittering updates)
  setInterval(() => {
    if (!state.isIndexing) return;

    if (state.embeddedCount < state.totalCount) {
      const increment = Math.floor(Math.random() * 8) + 4;
      state.embeddedCount = Math.min(state.totalCount, state.embeddedCount + increment);
      
      const pct = ((state.embeddedCount / state.totalCount) * 100).toFixed(1);
      dom.statEmbeddedCount.textContent = state.embeddedCount.toLocaleString();
      dom.winProgressFill.style.width = `${pct}%`;
      dom.progressPercentageText.textContent = `${pct}% completed`;
    }
  }, 420);

  // Rotate sample processing file periodically without flashing
  let currentFileIndex = 0;
  setInterval(() => {
    if (!state.isIndexing) return;
    currentFileIndex = (currentFileIndex + 1) % sampleIndexFiles.length;
    dom.currentProcessingPath.textContent = `Processing: ${sampleIndexFiles[currentFileIndex]}`;
  }, 1600);

  // Scope Radio Buttons ("Include whole PC" vs "Specify directories")
  dom.radioScopeAll.addEventListener('change', () => {
    state.scopeMode = 'all';
    dom.includedFoldersContainer.style.display = 'none';
    showToast('Indexing Scope Updated', 'Scanning all connected storage drives (C:\\, D:\\)');
  });

  dom.radioScopeCustom.addEventListener('change', () => {
    state.scopeMode = 'custom';
    dom.includedFoldersContainer.style.display = 'block';
    showToast('Indexing Scope Updated', 'Scanning only specified directories');
  });

  // Add Directory to Included Folders
  dom.btnAddIncludedFolder.addEventListener('click', () => {
    const defaultPaths = [
      'D:\\Projects\\Assets\\Textures',
      'C:\\Users\\usy\\Downloads\\MediaArchive',
      'D:\\Design\\Figma_Exports'
    ];
    const newPath = defaultPaths[Math.floor(Math.random() * defaultPaths.length)];
    
    // Check if already in list
    if (state.includedDirs.some(d => d.path === newPath)) {
      showToast('Directory already added', newPath);
      return;
    }

    state.includedDirs.push({ path: newPath, count: '1,420 files' });
    renderDirectories();
    showToast('Directory Added', `Added ${newPath} to inclusion scan list`, 'success');
  });

  // Add Directory to Excluded Folders (Always visible section)
  dom.btnAddExcludedFolder.addEventListener('click', () => {
    const sampleExcludes = [
      'C:\\Users\\usy\\Downloads\\Temp',
      'D:\\SteamLibrary\\**',
      '**\\dist\\**',
      'C:\\Users\\usy\\Pictures\\Private_Vault'
    ];
    const newExclude = sampleExcludes[Math.floor(Math.random() * sampleExcludes.length)];

    if (state.excludedDirs.some(d => d.path === newExclude)) {
      showToast('Exclusion already active', newExclude);
      return;
    }

    state.excludedDirs.push({ path: newExclude, desc: 'User Exclusion', system: false });
    renderDirectories();
    showToast('Exclusion Rule Added', `Skipping: ${newExclude}`, 'success');
  });

  function renderDirectories() {
    // Render Included
    dom.includedDirectoriesList.innerHTML = '';
    state.includedDirs.forEach((dir, index) => {
      const li = document.createElement('li');
      li.className = 'directory-item';
      li.innerHTML = `
        <div class="dir-icon">📁</div>
        <div class="dir-path" title="${dir.path}">${dir.path}</div>
        <span class="dir-badge">${dir.count}</span>
        <button class="dir-btn-remove" title="Remove directory">✕</button>
      `;
      li.querySelector('.dir-btn-remove').addEventListener('click', () => {
        state.includedDirs.splice(index, 1);
        renderDirectories();
      });
      dom.includedDirectoriesList.appendChild(li);
    });

    // Render Excluded (Always Visible)
    dom.excludedDirectoriesList.innerHTML = '';
    state.excludedDirs.forEach((item, index) => {
      const li = document.createElement('li');
      li.className = `directory-item ${item.system ? 'is-system' : ''}`;
      li.innerHTML = `
        <div class="dir-icon">🚫</div>
        <div class="dir-path" title="${item.path}">${item.path}</div>
        <span class="dir-badge ${item.system ? 'system-badge' : ''}">${item.desc}</span>
        <button class="dir-btn-remove" ${item.system ? 'disabled title="System protected rule"' : 'title="Remove rule"'}>
          ${item.system ? '🔒' : '✕'}
        </button>
      `;
      if (!item.system) {
        li.querySelector('.dir-btn-remove').addEventListener('click', () => {
          state.excludedDirs.splice(index, 1);
          renderDirectories();
          showToast('Exclusion Removed', `Removed ${item.path}`);
        });
      }
      dom.excludedDirectoriesList.appendChild(li);
    });
  }


  // =========================================================================
  // 12. TAB 2: KEYBIND CONFIGURATION & RECORDING
  // =========================================================================
  dom.toggleKeybindEnabled.addEventListener('change', (e) => {
    state.keybindEnabled = e.target.checked;
    dom.keybindStatusText.textContent = state.keybindEnabled ? 'Enabled' : 'Disabled';
    dom.keybindHintMsg.textContent = state.keybindEnabled 
      ? 'Press this combination anywhere to summon or hide the search pill.' 
      : 'Global shortcut is disabled. Use tray icon or desktop shortcut to summon search.';
    
    showToast(
      state.keybindEnabled ? 'Keybind Enabled' : 'Keybind Disabled',
      state.keybindEnabled ? `Shortcut: ${state.keybind.display.join(' + ')}` : 'App will not react to shortcut'
    );
  });

  dom.btnRecordKeybind.addEventListener('click', () => {
    if (state.isRecordingKeybind) return;
    state.isRecordingKeybind = true;
    dom.recordKeybindText.textContent = 'Press any key combination...';
    dom.btnRecordKeybind.classList.add('win-btn-primary');
    showToast('Record Shortcut', 'Press the key combination you want to assign (e.g. Ctrl + Shift + Space)');
  });

  dom.btnResetKeybind.addEventListener('click', () => {
    state.keybind = {
      altKey: true,
      ctrlKey: false,
      shiftKey: false,
      metaKey: false,
      key: ' ',
      display: ['Alt', 'Space']
    };
    updateKeybindDisplay();
    showToast('Keybind Reset', 'Restored default shortcut: Alt + Space');
  });

  function updateKeybindDisplay() {
    dom.keyCap1.textContent = state.keybind.display[0] || 'Alt';
    dom.keyCap2.textContent = state.keybind.display[1] || 'Space';
    dom.topbarKeybindHint.textContent = state.keybind.display[0] || 'Alt';
    dom.traySearchHotkey.textContent = state.keybind.display.join('+');
  }

  // Global Keydown Listener for shortcut invocation & recording
  window.addEventListener('keydown', (e) => {
    // 1. Keybind Recording Mode
    if (state.isRecordingKeybind) {
      e.preventDefault();
      // Ignore bare modifiers alone
      if (['Control', 'Alt', 'Shift', 'Meta'].includes(e.key)) return;

      const displayParts = [];
      if (e.ctrlKey) displayParts.push('Ctrl');
      if (e.altKey) displayParts.push('Alt');
      if (e.shiftKey) displayParts.push('Shift');
      if (e.metaKey) displayParts.push('Win');

      let mainKey = e.key;
      if (mainKey === ' ') mainKey = 'Space';
      if (mainKey.length === 1) mainKey = mainKey.toUpperCase();
      displayParts.push(mainKey);

      state.keybind = {
        altKey: e.altKey,
        ctrlKey: e.ctrlKey,
        shiftKey: e.shiftKey,
        metaKey: e.metaKey,
        key: e.key,
        display: displayParts
      };

      state.isRecordingKeybind = false;
      dom.recordKeybindText.textContent = 'Record New Keybind';
      dom.btnRecordKeybind.classList.remove('win-btn-primary');
      updateKeybindDisplay();
      showToast('Shortcut Updated', `New invocation keybind: ${displayParts.join(' + ')}`, 'success');
      return;
    }

    // 2. Escape Key Handling
    if (e.key === 'Escape') {
      if (dom.searchAutocomplete && dom.searchAutocomplete.style.display !== 'none') {
        hideAutocomplete();
        return;
      }
      if (dom.imageTagDialog && dom.imageTagDialog.style.display !== 'none') {
        dom.imageTagDialog.style.display = 'none';
        state.currentModalMedia = null;
        return;
      }
      if (dom.tagRibbonBar && dom.tagRibbonBar.style.display !== 'none') {
        dom.tagRibbonBar.style.display = 'none';
        state.stampTag = null;
        renderMediaResults();
        return;
      }
      if (state.isFullscreen) {
        toggleFullscreen();
        return;
      }
      if (dom.trayContextMenu.style.display !== 'none') {
        dom.trayContextMenu.style.display = 'none';
        return;
      }
      if (dom.settingsWindow.style.display !== 'none') {
        dom.settingsWindow.style.display = 'none';
        return;
      }
      if (dom.terminalWindow.style.display !== 'none') {
        dom.terminalWindow.style.display = 'none';
        return;
      }
      if (state.isPillVisible) {
        hidePill();
        return;
      }
    }

    // 3. Test Global Hotkey Trigger
    if (!state.keybindEnabled) return;

    const matchesAlt = state.keybind.altKey ? e.altKey : !e.altKey;
    const matchesCtrl = state.keybind.ctrlKey ? e.ctrlKey : !e.ctrlKey;
    const matchesShift = state.keybind.shiftKey ? e.shiftKey : !e.shiftKey;
    const matchesKey = e.key.toLowerCase() === state.keybind.key.toLowerCase() ||
                       (state.keybind.key === ' ' && e.code === 'Space');

    if (matchesAlt && matchesCtrl && matchesShift && matchesKey) {
      e.preventDefault();
      togglePill();
    }
  });


  // =========================================================================
  // 13. MODEL SELECTION
  // =========================================================================
  dom.selectModel.addEventListener('change', (e) => {
    state.model = e.target.value;
    const spec = state.modelSpecs[state.model];
    dom.modelDim.textContent = spec.dim;
    dom.modelDevice.textContent = spec.device;
    dom.modelVram.textContent = spec.vram;
    renderMediaResults();
    showToast('AI Model Changed', `Switched embedding model to ${spec.name} (${spec.dim})`);
  });


  // =========================================================================
  // 14. PYTHON VIRTUAL ENVIRONMENT & SIMULATED TERMINAL
  // =========================================================================
  dom.btnOpenTerminal.addEventListener('click', () => {
    openTerminalWindow();
  });

  dom.btnUpdateVenv.addEventListener('click', () => {
    showToast('Updating Virtualenv', 'Running pip install --upgrade -r requirements.txt...');
    openTerminalWindow();
    appendTerminalOutput([
      '(.venv) C:\\Users\\usy\\Documents\\Projects\\usyseek> pip install --upgrade -r requirements.txt',
      'Requirement already satisfied: torch==2.3.1+cu121',
      'Requirement already satisfied: open-clip-torch==2.24.0',
      'Collecting pillow>=10.3.0',
      '  Downloading pillow-10.4.0-cp311-cp311-win_amd64.whl (2.6 MB)',
      'Installing collected packages: pillow',
      'Successfully installed pillow-10.4.0',
      'Done. Virtual environment is up to date.'
    ]);
  });

  dom.btnRecreateVenv.addEventListener('click', () => {
    if (confirm('Rebuild .venv from scratch? This will recreate the virtual environment and reinstall torch & clip.')) {
      showToast('Rebuilding Venv', 'Executing python -m venv .venv...');
      openTerminalWindow();
      appendTerminalOutput([
        '(.venv) C:\\Users\\usy\\Documents\\Projects\\usyseek> python -m venv .venv --clear',
        'Creating virtual environment at C:\\Users\\usy\\Documents\\Projects\\usyseek\\.venv...',
        'Installing pip, setuptools, wheel...',
        'Installing PyTorch CUDA 12.1 and OpenCLIP bindings...',
        'Successfully initialized clean environment (.venv).'
      ]);
    }
  });

  dom.btnDeleteVenv.addEventListener('click', () => {
    if (confirm('Are you sure you want to delete the .venv directory?')) {
      state.venvActive = false;
      dom.venvStatusBadge.className = 'status-badge';
      dom.venvStatusBadge.style.color = '#ff8585';
      dom.venvStatusBadge.style.background = 'rgba(255, 80, 80, 0.15)';
      dom.venvStatusBadge.innerHTML = `<span class="badge-dot"></span> .venv Deleted`;
      showToast('Virtualenv Removed', 'Python virtual environment folder removed. Recreate to restore AI search features.');
    }
  });

  // Simulated Terminal Window
  function openTerminalWindow() {
    dom.terminalWindow.style.display = 'flex';
    setTimeout(() => dom.termInput.focus(), 100);
  }

  dom.btnTerminalClose.addEventListener('click', () => {
    dom.terminalWindow.style.display = 'none';
  });

  function appendTerminalOutput(lines) {
    lines.forEach(text => {
      const line = document.createElement('div');
      line.className = 'term-line';
      line.textContent = text;
      dom.termHistory.appendChild(line);
    });
    dom.terminalBody.scrollTop = dom.terminalBody.scrollHeight;
  }

  dom.termInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      const cmd = dom.termInput.value.trim();
      dom.termInput.value = '';

      const cmdEcho = document.createElement('div');
      cmdEcho.className = 'term-line';
      cmdEcho.innerHTML = `<span class="term-prompt">(.venv) C:\\Users\\usy\\Documents\\Projects\\usyseek&gt;</span> ${cmd}`;
      dom.termHistory.appendChild(cmdEcho);

      if (!cmd) return;

      const lower = cmd.toLowerCase();
      let responses = [];

      if (lower === 'cls' || lower === 'clear') {
        dom.termHistory.innerHTML = '';
        return;
      } else if (lower === 'help') {
        responses = [
          'usyseek CLI mock commands:',
          '  python -V                 Show python version',
          '  pip list                  List installed packages in .venv',
          '  python index.py --stats   Show current embedding index database stats',
          '  cls                       Clear terminal screen',
          '  exit                      Close terminal window'
        ];
      } else if (lower.includes('python -v') || lower === 'python --version') {
        responses = ['Python 3.11.8 (tags/v3.11.8:d79d9ee, Feb  6 2024, 15:43:21) [MSC v.1937 64 bit (AMD64)] on win32'];
      } else if (lower.includes('pip list')) {
        responses = [
          'Package          Version',
          '---------------- -----------',
          'faiss-cpu        1.8.0',
          'open-clip-torch  2.24.0',
          'pillow           10.4.0',
          'pip              24.0',
          'torch            2.3.1+cu121',
          'torchvision      0.18.1+cu121',
          'transformers     4.41.2'
        ];
      } else if (lower.includes('--stats') || lower.includes('stats')) {
        responses = [
          `Index Engine: usyseek FAISS Index (FlatIP)`,
          `Indexed Files: ${state.embeddedCount} / ${state.totalCount}`,
          `Model: ${state.modelSpecs[state.model].name}`,
          `Embedding Dimension: ${state.modelSpecs[state.model].dim}`,
          `Index Size on Disk: 48.6 MB`
        ];
      } else if (lower === 'exit') {
        dom.terminalWindow.style.display = 'none';
        return;
      } else {
        responses = [`'${cmd}' executed in (.venv). Command finished with exit code 0.`];
      }

      appendTerminalOutput(responses);
    }
  });


  // =========================================================================
  // 15. SYSTEM TRAY & WIN32 CONTEXT MENU
  // =========================================================================
  // Left Click on Tray: Open/Toggle Search Pill
  dom.trayAppIcon.addEventListener('click', (e) => {
    e.stopPropagation();
    dom.trayContextMenu.style.display = 'none';
    togglePill();
  });

  // Right Click on Tray: Show Win32 Context Menu
  dom.trayAppIcon.addEventListener('contextmenu', (e) => {
    e.preventDefault();
    e.stopPropagation();

    // Position menu above tray icon
    const rect = dom.trayAppIcon.getBoundingClientRect();
    dom.trayContextMenu.style.left = `${Math.max(10, rect.left - 170)}px`;
    dom.trayContextMenu.style.bottom = `${window.innerHeight - rect.top + 6}px`;
    dom.trayContextMenu.style.display = 'block';
  });

  // Tray Menu Items
  dom.trayMenuOpenSearch.addEventListener('click', () => {
    dom.trayContextMenu.style.display = 'none';
    showPill();
  });

  dom.trayMenuOpenInterface.addEventListener('click', () => {
    dom.trayContextMenu.style.display = 'none';
    openSettingsWindow();
  });

  dom.trayMenuToggleIndex.addEventListener('click', () => {
    dom.trayContextMenu.style.display = 'none';
    toggleIndexing();
    showToast(
      state.isIndexing ? 'Indexing Resumed' : 'Indexing Paused',
      state.isIndexing ? 'Active scanning worker started' : 'Background indexing paused'
    );
  });

  dom.trayMenuQuit.addEventListener('click', () => {
    dom.trayContextMenu.style.display = 'none';
    hidePill();
    dom.settingsWindow.style.display = 'none';
    dom.terminalWindow.style.display = 'none';
    showToast('usyseek Exited', 'Background service stopped');
  });

  // Hide context menu when clicking anywhere else
  window.addEventListener('click', (e) => {
    if (!e.target.closest('#trayContextMenu')) {
      dom.trayContextMenu.style.display = 'none';
    }
  });


  // =========================================================================
  // 16. DESKTOP SIMULATION HELPERS & TIME
  // =========================================================================
  function updateClock() {
    const now = new Date();
    let hours = now.getHours();
    const minutes = String(now.getMinutes()).padStart(2, '0');
    const ampm = hours >= 12 ? 'PM' : 'AM';
    hours = hours % 12 || 12;

    dom.trayTime.textContent = `${hours}:${minutes} ${ampm}`;
    dom.trayDate.textContent = `${now.getMonth() + 1}/${now.getDate()}/${now.getFullYear()}`;
  }
  updateClock();
  setInterval(updateClock, 30000);

  // Toggle Focused Mode vs Desktop Mode
  dom.btnToggleDesktopMode.addEventListener('click', () => {
    document.body.classList.toggle('focused-mode');
    const isFocused = document.body.classList.contains('focused-mode');
    dom.desktopModeText.textContent = isFocused ? 'Desktop Mode' : 'Focused Mode';
    showToast(
      isFocused ? 'Focused View' : 'Windows 11 Desktop View',
      isFocused ? 'Background desktop icons minimized' : 'Full desktop with taskbar active'
    );
  });

  // Reset Layout Button
  dom.btnResetLayout.addEventListener('click', () => {
    resetPillWidth();
    savedPillPosition = null;
    dom.spotlightWrapper.style.top = '12%';
    dom.spotlightWrapper.style.left = '50%';
    dom.spotlightWrapper.style.transform = 'translateX(-50%)';
    dom.settingsWindow.style.top = '14%';
    dom.settingsWindow.style.left = '25%';
    dom.settingsWindow.style.transform = 'none';
    showPill();
    showToast('Layout Reset', 'Pill and window restored to default positions');
  });


  // =========================================================================
  // 17. INITIALIZATION
  // =========================================================================
  renderMediaResults();
  updateIndexingUI();
  renderDirectories();
  updateKeybindDisplay();

  // Welcome Toast on startup
  setTimeout(() => {
    showToast(
      'usyseek Ready',
      'Press Alt + Space or click the tray icon to toggle Spotlight Search.',
      'info'
    );
  }, 400);

});
