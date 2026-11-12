// Gate fuse_ui_a11y_tree_dump: roles, names, states and values of the sample tree (D7: tree now, AccessKit later).
#include "test_util.hpp"

using namespace fuse::ui;
using namespace fuse::ui::test;

int main() {
    UiContext ui({.viewport = {488, 400}, .font = &font()});
    widgets::install(ui);
    buildSample(ui);
    ui.setA11y(ui.find("card"), A11yRole::Group, "Settings");
    ui.update();
    UiTestDriver(ui).click("name");
    std::string dump;
    ui.dumpA11y(dump);
    const std::string want =
        "window\n"
        "  group \"Settings\"\n"
        "    text \"FUSE UI \xE2\x80\x94 Tier A widgets\"\n"
        "    text \"Retained core, CSS-subset themes, CPU reference rasteriser.\"\n"
        "    button \"Apply\" focusable\n"
        "    button \"Cancel\" focusable\n"
        "    button \"Tool\" focusable\n"
        "    link \"Open docs\" focusable\n"
        "    separator\n"
        "    checkbox \"Shadows\" focusable checked\n"
        "    checkbox \"Bloom\" focusable\n"
        "    switch \"V-Sync\" focusable checked\n"
        "    radio \"Low\" focusable\n"
        "    radio \"High\" focusable checked\n"
        "    radio \"Ultra\" focusable\n"
        "    text \"Exposure\"\n"
        "    slider focusable value=0.62 range=[0,1]\n"
        "    dial focusable value=35 range=[0,100]\n"
        "    spinbox focusable value=144 range=[0,240]\n"
        "    combobox \"Vulkan\nCPU raster\nNull\" focusable\n"
        "    textfield \"player_one\" focusable focused\n"
        "    progressbar value=0.7 range=[0,1]\n"
        "    text \"Scroll\"\n"
        "    scrollbar value=0.3 range=[0,1]\n";
    if (dump != want) std::fprintf(stderr, "--- got ---\n%s--- want ---\n%s", dump.c_str(), want.c_str());
    UI_CHECK(dump == want);
    return finish("fuse_ui_a11y_tree_dump");
}
