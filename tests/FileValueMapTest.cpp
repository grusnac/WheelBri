// SPDX-License-Identifier: GPL-3.0-or-later
#include "wheelbri/FileValueMap.hpp"

#include "wheelbri/IniDocument.hpp"

#include <gtest/gtest.h>

#include <string>

namespace wheelbri {
namespace {

// As GRID's ActionMap/actionMapPreset1.xml: CRLF, tab indented, `attr = "v"`, comments, one action space indented.
const std::string kFile =
    "<ActionMap deviceType=\"Logitech G25 Racing Wheel USB\" priority=\"1\" >\r\n"
    "\t<!-- Replay Actions -->\r\n"
    "\t<!-- <Action actionName = \"Replay Pause\" > -->\r\n"
    "  <Action actionName = \"Boost\" >\r\n"
    "    <Axis axisName = \"win_key_b\" deviceName = \"Keyboard\" />\r\n"
    "  </Action>\r\n"
    "\t<Action actionName = \"Replay Pause\" >\r\n"
    "\t\t<Axis axisName = \"win_key_f1\" deviceName = \"Keyboard\" />\r\n"
    "\t</Action>\r\n"
    "\t<Action actionName = \"Replay Exit\" >\r\n"
    "\t\t<Axis axisName = \"win_key_escape\" deviceName = \"Keyboard\" />\r\n"
    "\t</Action>\r\n"
    "</ActionMap>\r\n";

FileValueMap mapFrom(const std::string& section) {
    const auto parsed = IniDocument::parse("[game_files]\n" + section);
    EXPECT_TRUE(parsed.errors.empty());
    const auto it = parsed.document.sections().find("game_files");
    ParsedFileValueMap map = parseFileValueMap(it == parsed.document.sections().end()
                                                   ? std::map<std::string, std::string>{}
                                                   : it->second);
    EXPECT_TRUE(map.problems.empty()) << (map.problems.empty() ? "" : map.problems.front());
    return map.map;
}

std::vector<std::string> problemsOf(const std::string& section) {
    const auto parsed = IniDocument::parse("[game_files]\n" + section);
    return parseFileValueMap(parsed.document.sections().at("game_files")).problems;
}

const std::string kMap =
    "file = ActionMap/actionMapPreset1.xml\n"
    "note = Replay controls on the wheel\n"
    "insert.1 = Action[actionName=Replay Pause] | end | <Axis axisName = \"win_con_di_button17\" deviceName = \"G25\" />\n"
    "insert.1.note = A pauses the replay\n"
    "insert.2 = Action[actionName=Replay Exit] | end | <Axis axisName = \"win_con_di_button15\" deviceName = \"G25\" />\n"
    "insert.2.note = Y leaves the replay\n"
    "insert.3 = Action[actionName=Replay Exit] | end | <Axis axisName = \"win_con_di_button3\" deviceName = \"G25\" />\n"
    "insert.3.note = Red 4 leaves the replay\n"
    "insert.4 = ActionMap | end | <Action actionName = \"Menu Start Button\" ><Axis axisName = \"win_con_di_button3\" /></Action>\n"
    "insert.4.note = A new action\n";

TEST(FileValueMap, ReadsTheSectionInTheOrderOfItsNumbers) {
    std::string section = kMap;
    for (int n = 5; n <= 10; ++n) {
        section += "insert." + std::to_string(n) + " = ActionMap | start | <X n=\"" + std::to_string(n) + "\"/>\n";
        section += "insert." + std::to_string(n) + ".note = number " + std::to_string(n) + "\n";
    }
    const FileValueMap map = mapFrom(section);
    EXPECT_EQ(map.file, "ActionMap/actionMapPreset1.xml");
    EXPECT_EQ(map.note, "Replay controls on the wheel");
    ASSERT_EQ(map.lines.size(), 10U);
    EXPECT_EQ(map.lines[0].parent, "Action[actionName=Replay Pause]");
    EXPECT_EQ(map.lines[0].position, FileValueLine::Position::End);
    EXPECT_EQ(map.lines[0].note, "A pauses the replay");
    EXPECT_EQ(map.lines[9].value, "<X n=\"10\"/>");   // 10 after 9, not after 1
    const FileValueMap after = mapFrom(kMap + "insert.5 = ActionMap | after: Action[actionName=Boost] | <X/>\n"
                                              "insert.5.note = after Boost\n");
    EXPECT_EQ(after.lines[4].position, FileValueLine::Position::After);
    EXPECT_EQ(after.lines[4].after, "Action[actionName=Boost]");
}

TEST(FileValueMap, SaysWhatIsWrongWithTheSection) {
    const auto has = [](const std::vector<std::string>& problems, const std::string& part) {
        for (const std::string& p : problems) {
            if (p.find(part) != std::string::npos) return true;
        }
        return false;
    };
    EXPECT_TRUE(has(problemsOf(kMap + "colour = red\n"), "colour: unknown key"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | end | <X/>\n"), "insert.5.note go together"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.6 = ActionMap | end | <X/>\ninsert.6.note = x\n"), "without gaps"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.05 = ActionMap | end | <X/>\n"), "insert.<1.."));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.33 = ActionMap | end | <X/>\n"), "insert.<1.."));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | middle | <X/>\ninsert.5.note = x\n"), "position"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | after: | <X/>\ninsert.5.note = x\n"), "position"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = Action[name] | end | <X/>\ninsert.5.note = x\n"), "parent"));
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | end\ninsert.5.note = x\n"), "expected <parent key>"));
    for (const std::string_view value : {"<X/><Y/>", "<X>", "text", "<X/> tail", "<X a=\"1/>", "</X>", "<!-- c --><X/>"}) {
        EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | end | " + std::string(value) + "\ninsert.5.note = x\n"),
                        "one element on one line"))
            << value;
    }
    EXPECT_TRUE(has(problemsOf(kMap + "insert.5 = ActionMap | end | <Action actionName = \"Menu Start Button\" >"
                                      "<Axis  axisName = \"win_con_di_button3\" /></Action>\ninsert.5.note = x\n"),
                    "the same line twice"));
    for (const std::string_view file : {"../x.xml", "/x.xml", "C:/x.xml", "a\\x.xml", "a//x.xml", "./x.xml"}) {
        std::string section = kMap;
        section.replace(section.find("ActionMap/actionMapPreset1.xml"), 30, file);
        EXPECT_TRUE(has(problemsOf(section), "relative to the game's folder")) << file;
    }
    EXPECT_TRUE(has(problemsOf("note = x\n"), "go together"));
    EXPECT_TRUE(has(problemsOf("file = a.xml\ninsert.1 = ActionMap | end | <X/>\ninsert.1.note = x\n"), "go together"));
}

TEST(FileValueMap, AddPutsTheLinesInOnceAndRemoveTakesExactlyThoseOut) {
    const FileValueMap map = mapFrom(kMap);
    const FileValueState before = fileValueState(kFile, map);
    EXPECT_TRUE(before.none());
    const FileValueEdit added = addFileValues(kFile, map);
    ASSERT_TRUE(added.problems.empty()) << added.problems.front();
    EXPECT_EQ(added.changed, 4U);
    EXPECT_EQ(added.text,
              "<ActionMap deviceType=\"Logitech G25 Racing Wheel USB\" priority=\"1\" >\r\n"
              "\t<!-- Replay Actions -->\r\n"
              "\t<!-- <Action actionName = \"Replay Pause\" > -->\r\n"
              "  <Action actionName = \"Boost\" >\r\n"
              "    <Axis axisName = \"win_key_b\" deviceName = \"Keyboard\" />\r\n"
              "  </Action>\r\n"
              "\t<Action actionName = \"Replay Pause\" >\r\n"
              "\t\t<Axis axisName = \"win_key_f1\" deviceName = \"Keyboard\" />\r\n"
              "\t\t<Axis axisName = \"win_con_di_button17\" deviceName = \"G25\" />\r\n"
              "\t</Action>\r\n"
              "\t<Action actionName = \"Replay Exit\" >\r\n"
              "\t\t<Axis axisName = \"win_key_escape\" deviceName = \"Keyboard\" />\r\n"
              "\t\t<Axis axisName = \"win_con_di_button15\" deviceName = \"G25\" />\r\n"
              "\t\t<Axis axisName = \"win_con_di_button3\" deviceName = \"G25\" />\r\n"
              "\t</Action>\r\n"
              "  <Action actionName = \"Menu Start Button\" ><Axis axisName = \"win_con_di_button3\" /></Action>\r\n"
              "</ActionMap>\r\n");
    EXPECT_TRUE(fileValueState(added.text, map).all());
    const FileValueEdit again = addFileValues(added.text, map);
    EXPECT_TRUE(again.problems.empty());
    EXPECT_EQ(again.changed, 0U);
    EXPECT_EQ(again.text, added.text);
    const FileValueEdit removed = removeFileValues(added.text, map);
    ASSERT_TRUE(removed.problems.empty());
    EXPECT_EQ(removed.changed, 4U);
    EXPECT_EQ(removed.text, kFile);
    const FileValueEdit removedAgain = removeFileValues(kFile, map);
    EXPECT_EQ(removedAgain.changed, 0U);
    EXPECT_EQ(removedAgain.text, kFile);
}

TEST(FileValueMap, LinesAddedByHandWithOtherSpacingAreAlreadyThere) {
    const FileValueMap map = mapFrom(kMap);
    std::string file = kFile;
    file.insert(file.find("\t</Action>\r\n\t<Action actionName = \"Replay Exit\""),
                "    <Axis  axisName = \"win_con_di_button17\"\tdeviceName = \"G25\"   />   \r\n");
    const FileValueState state = fileValueState(file, map);
    ASSERT_TRUE(state.problems.empty());
    EXPECT_EQ(state.presentCount, 1U);
    EXPECT_TRUE(state.present[0]);
    EXPECT_FALSE(state.all());
    EXPECT_FALSE(state.none());
    const FileValueEdit added = addFileValues(file, map);
    EXPECT_EQ(added.changed, 3U);
    // Remove takes the hand-added line too (it is one of the map's), and nothing else.
    EXPECT_EQ(removeFileValues(added.text, map).text, kFile);
}

TEST(FileValueMap, OnlyChildLinesOfTheParentCount) {
    const FileValueMap map = mapFrom(kMap);
    std::string file = kFile;
    // The same line in another action, and in a comment, is not the one asked.
    file.insert(file.find("  </Action>\r\n"), "    <Axis axisName = \"win_con_di_button17\" deviceName = \"G25\" />\r\n");
    file.insert(file.find("\t</Action>\r\n"),
                "\t\t<!-- <Axis axisName = \"win_con_di_button17\" deviceName = \"G25\" /> -->\r\n");
    EXPECT_TRUE(fileValueState(file, map).none());
    const FileValueEdit removed = removeFileValues(file, map);
    EXPECT_EQ(removed.changed, 0U);
    EXPECT_EQ(removed.text, file);
}

TEST(FileValueMap, StartEndAndAfterKeepTheMapsOrder) {
    const FileValueMap map = mapFrom(
        "file = a.xml\nnote = n\n"
        "insert.1 = ActionMap | start | <A n=\"1\"/>\ninsert.1.note = x\n"
        "insert.2 = ActionMap | start | <A n=\"2\"/>\ninsert.2.note = x\n"
        "insert.3 = ActionMap | after:Action[actionName = Boost] | <A n=\"3\"/>\ninsert.3.note = x\n"
        "insert.4 = ActionMap | after:Action[actionName=Boost] | <A n=\"4\"/>\ninsert.4.note = x\n");
    const FileValueEdit added = addFileValues(kFile, map);
    ASSERT_TRUE(added.problems.empty()) << added.problems.front();
    const auto at = [&added](const std::string& part) { return added.text.find(part); };
    EXPECT_LT(at("<ActionMap"), at("  <A n=\"1\"/>\r\n"));
    EXPECT_LT(at("<A n=\"1\"/>"), at("<A n=\"2\"/>"));
    EXPECT_LT(at("<A n=\"2\"/>"), at("<!-- Replay Actions"));
    EXPECT_LT(at("  </Action>\r\n"), at("  <A n=\"3\"/>\r\n"));
    EXPECT_LT(at("<A n=\"3\"/>"), at("<A n=\"4\"/>"));
    EXPECT_LT(at("<A n=\"4\"/>"), at("\t<Action actionName = \"Replay Pause\""));
    EXPECT_EQ(removeFileValues(added.text, map).text, kFile);
}

TEST(FileValueMap, AnyProblemChangesNothing) {
    const auto refused = [](const std::string& file, const FileValueMap& map) {
        const FileValueEdit add = addFileValues(file, map);
        const FileValueEdit remove = removeFileValues(file, map);
        const FileValueState state = fileValueState(file, map);
        return !add.problems.empty() && add.text.empty() && add.changed == 0 && !remove.problems.empty() &&
               remove.text.empty() && !state.problems.empty() && state.presentCount == 0;
    };
    const FileValueMap map = mapFrom(kMap);
    std::string noExit = kFile;
    noExit.replace(noExit.find("Replay Exit"), 11, "Replay Quit");
    EXPECT_TRUE(refused(noExit, map));   // a parent not there: nothing at all, not the other lines either
    std::string twoPause = kFile;
    twoPause.replace(twoPause.find("Replay Exit"), 11, "Replay Pause");
    EXPECT_TRUE(refused(twoPause, map));
    for (const std::string& broken : {std::string("<ActionMap>\r\n\t<Action>\r\n</ActionMap>\r\n"),
                                     std::string("<ActionMap>\r\n<!-- open\r\n</ActionMap>\r\n"),
                                     std::string("<ActionMap a=\"1>\r\n</ActionMap>\r\n"), std::string("<ActionMap"),
                                     std::string(), std::string("plain text")}) {
        EXPECT_TRUE(refused(broken, map)) << broken;
    }
    // A parent on one line has no line to put a child on.
    const FileValueMap oneLine = mapFrom("file = a.xml\nnote = n\ninsert.1 = B | end | <X/>\ninsert.1.note = x\n");
    EXPECT_FALSE(addFileValues("<A>\n<B></B>\n</A>\n", oneLine).problems.empty());
    EXPECT_FALSE(addFileValues("<A>\n<B/>\n</A>\n", oneLine).problems.empty());
    // After a child that is not there.
    const FileValueMap afterMissing =
        mapFrom("file = a.xml\nnote = n\ninsert.1 = A | after:C | <X/>\ninsert.1.note = x\n");
    EXPECT_FALSE(addFileValues("<A>\n<B/>\n</A>\n", afterMissing).problems.empty());
}

TEST(FileValueMap, KeepsTheFilesLineEndingsAndIndentation) {
    const FileValueMap map = mapFrom("file = a.xml\nnote = n\ninsert.1 = B | end | <X/>\ninsert.1.note = x\n"
                                     "insert.2 = C | end | <Y/>\ninsert.2.note = x\n");
    EXPECT_EQ(addFileValues("<A>\n  <B>\n      <Z/>\n  </B>\n  <C>\n  </C>\n</A>", map).text,
              "<A>\n  <B>\n      <Z/>\n      <X/>\n  </B>\n  <C>\n    <Y/>\n  </C>\n</A>");
    const FileValueMap root = mapFrom("file = a.xml\nnote = n\ninsert.1 = A | after:B | <X/>\ninsert.1.note = x\n");
    EXPECT_EQ(addFileValues("<A>\r\n\t<B/>", root).problems.size(), 1U);   // <A> not closed
    EXPECT_EQ(addFileValues("<A>\r\n\t<B/>\r\n</A>", root).text, "<A>\r\n\t<B/>\r\n\t<X/>\r\n</A>");
    const FileValueMap last = mapFrom("file = a.xml\nnote = n\ninsert.1 = A | end | <X/>\ninsert.1.note = x\n");
    EXPECT_EQ(addFileValues("<A>\n</A>", last).text, "<A>\n\t<X/>\n</A>");   // no child: one step in
}

TEST(FileValueMap, CollapsesRunsOfSpacesAndTabs) {
    EXPECT_EQ(collapseSpaces("  <A  b = \"1\"\t\t/>  "), "<A b = \"1\" />");
    EXPECT_EQ(collapseSpaces(""), "");
    EXPECT_EQ(collapseSpaces(" \t "), "");
}

}  // namespace
}  // namespace wheelbri
