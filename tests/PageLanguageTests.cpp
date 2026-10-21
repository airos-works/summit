#include "core/PageLanguage.h"
#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

int main()
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    const std::vector<std::pair<std::string, std::string>> samples = {
        {"en", "The public library opens every morning at nine. Visitors can borrow books, read newspapers and use the computers near the entrance. On Saturday a teacher will explain how to grow vegetables in a small garden. Children can draw pictures while their parents listen. Please bring a notebook and arrive a few minutes before the lesson begins. Everyone is welcome and there is no charge for attending."},
        {"de", "Die Stadtbibliothek öffnet jeden Morgen um neun Uhr. Besucher können Bücher ausleihen, Zeitungen lesen und die Computer neben dem Eingang benutzen. Am Samstag erklärt eine Lehrerin, wie man Gemüse in einem kleinen Garten anbaut. Kinder können Bilder malen, während ihre Eltern zuhören. Bitte bringen Sie ein Notizbuch mit und kommen Sie einige Minuten vor Beginn der Veranstaltung. Alle sind willkommen und die Teilnahme ist kostenlos."},
        {"fr", "La bibliothèque municipale ouvre chaque matin à neuf heures. Les visiteurs peuvent emprunter des livres, lire les journaux et utiliser les ordinateurs près de l'entrée. Samedi, une enseignante expliquera comment cultiver des légumes dans un petit jardin. Les enfants pourront dessiner pendant que leurs parents écoutent. Veuillez apporter un carnet et arriver quelques minutes avant le début de la présentation. Tout le monde est bienvenu et la participation est gratuite."},
        {"es", "La biblioteca municipal abre cada mañana a las nueve. Los visitantes pueden pedir libros prestados, leer periódicos y utilizar los ordenadores cerca de la entrada. El sábado una profesora explicará cómo cultivar verduras en un jardín pequeño. Los niños podrán dibujar mientras sus padres escuchan. Por favor traigan un cuaderno y lleguen unos minutos antes del comienzo de la presentación. Todos son bienvenidos y la participación es gratuita."},
        {"ru", "Городская библиотека открывается каждое утро в девять часов. Посетители могут брать книги, читать газеты и пользоваться компьютерами рядом со входом. В субботу учительница расскажет, как выращивать овощи в небольшом саду. Дети смогут рисовать, пока их родители слушают. Пожалуйста, принесите блокнот и приходите за несколько минут до начала занятия. Приглашаются все желающие, участие бесплатное."},
        {"ja", "市立図書館は毎朝九時に開館します。来館者は本を借りたり、新聞を読んだり、入口の近くにあるコンピューターを使ったりすることができます。土曜日には先生が小さな庭で野菜を育てる方法を説明します。子どもたちは保護者が話を聞いている間に絵を描くことができます。ノートを持参して、講座が始まる数分前にお越しください。どなたでも参加できます。参加費は無料です。"},
        {"zh-Hans", "市立图书馆每天早上九点开门。读者可以借阅书籍、阅读报纸，也可以使用入口附近的电脑。星期六，一位老师将介绍如何在小花园里种植蔬菜。家长听讲的时候，孩子们可以画画。请带上笔记本，并在讲座开始前几分钟到达。欢迎大家参加，活动完全免费。图书馆还提供适合不同年龄读者的阅读活动。如果您有任何问题，请向服务台的工作人员咨询。"},
        {"zh-Hant", "市立圖書館每天早上九點開門。讀者可以借閱書籍、閱讀報紙，也可以使用入口附近的電腦。星期六，一位老師將介紹如何在小花園裡種植蔬菜。家長聽講的時候，孩子們可以畫畫。請帶上筆記本，並在講座開始前幾分鐘到達。歡迎大家參加，活動完全免費。圖書館還提供適合不同年齡讀者的閱讀活動。如果您有任何問題，請向服務臺的工作人員諮詢。"}
    };
    for (const auto& [expected, text] : samples) {
        const auto detected = DetectPageLanguage(text);
        check(detected.code == expected && detected.reliable, "detect " + expected + " (got " + detected.code + ")");
        check(detected.percentage >= 80 && detected.textBytes >= 200, "dominant readable text " + expected);
        check(!PageNeedsTranslation(detected, expected), "same language " + expected);
        check(PageNeedsTranslation(detected, expected == "en" ? "de_DE" : "en_AU"), "different language " + expected);
    }
    check(!DetectPageLanguage("Bonjour").reliable, "short text does not cause a prompt");
    check(!DetectPageLanguage("").reliable, "empty page");
    check(!DetectPageLanguage(std::string(500, 'x')).reliable, "repeated characters are not prose");
    check(!DetectPageLanguage(samples[0].second + "\n" + samples[1].second).reliable, "bilingual page without a clear majority");
    check(DetectPageLanguage(samples[1].second + "\xff").code.empty(), "invalid UTF-8 rejected");
    check(DetectPageLanguage(samples[1].second + "\xe2\x82").code.empty(), "incomplete UTF-8 rejected");
    check(DetectPageLanguage(samples[1].second + "\xc0\xaf").code.empty(), "overlong UTF-8 rejected");
    check(DetectPageLanguage(std::string(kPageLanguageSampleLimit + 1, 'a')).code.empty(), "sample limit enforced");
    std::string longText;
    while (longText.size() + samples[0].second.size() <= kPageLanguageSampleLimit) longText += samples[0].second;
    check(DetectPageLanguage(longText).code == "en", "large bounded sample");

    for (const auto& [tag, expected] : std::vector<std::pair<std::string, std::string>> {
        {"en_AU", "en"}, {"EN-us", "en"}, {"en-Latn-GB", "en"}, {"de-DE-1996", "de"},
        {"fr", "fr"}, {"pt_BR", "pt"}, {"es-419", "es"}, {"fil_PH", "fil"},
        {"en-US-u-ca-gregory", "en"}, {"en-x-test", "en"}, {"iw_IL", "he"}, {"in", "id"}, {"ji", "yi"},
        {"zh_CN", "zh-Hans"}, {"zh_TW", "zh-Hant"}, {"zh-HK", "zh-Hant"}, {"zh-MO", "zh-Hant"},
        {"zh", "zh-Hans"}, {"zh-Hans-TW", "zh-Hans"}, {"zh-Hant-CN", "zh-Hant"}
    }) check(PageLanguageKey(tag) == expected, "locale " + tag);
    for (const auto& tag : {"", "C", "POSIX", "und", "mul", "zxx", "un", "xxx", "english", "123", "en--US", "en_", "_en", "en-x", "en-u", "en-u-x", "en-abc", "en-US.UTF-8", " en", "en/US"})
        check(PageLanguageKey(tag).empty(), std::string("reject locale ") + tag);
    check(PreferredPageLanguage({"fr_CA", "en_AU"}) == "fr", "first system preference is target");
    check(PreferredPageLanguage({"ja_JP", "en"}) == "ja", "do not substitute a supported service language");
    check(PreferredPageLanguage({"invalid", "en"}).empty(), "do not skip an invalid first preference");
    check(PreferredPageLanguage({}).empty(), "no invented target");
    check(!PageNeedsTranslation(DetectPageLanguage(samples[0].second), "en_AU"), "regional English is same language");
    check(!PageNeedsTranslation(DetectPageLanguage(samples[0].second), "en-Latn-US"), "explicit Latin script is same English");
    check(PageNeedsTranslation(DetectPageLanguage(samples[6].second), "zh_TW"), "different Chinese writing system");
    check(!PageNeedsTranslation(DetectPageLanguage(samples[0].second), ""), "unknown target");
    check(!PageNeedsTranslation(DetectPageLanguage("Bonjour"), "en"), "uncertain text has no offer");
    std::cout << checks << " page language checks passed\n";
}
