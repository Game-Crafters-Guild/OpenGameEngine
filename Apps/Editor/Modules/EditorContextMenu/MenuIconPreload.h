#pragma once

namespace GameEngine
{
    namespace MenuIcons
    {
        /**
         * @brief Load the editor's icon set before a menu asks to draw it.
         *
         * Menu rows name their images by path at the call site, so the first
         * row to want one would otherwise pay the texture cook mid-open —
         * cheap against a desktop disk, visible in a browser. The set is small
         * and staged next to the executable, so it is loaded whole rather than
         * guessed at.
         */
        void PreloadEditorIconAssets();
    }
}
