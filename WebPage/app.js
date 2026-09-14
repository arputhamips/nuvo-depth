(() => {
  "use strict";

  const config = window.NUVO_STORE || {};
  const colorNames = { white: "White", black: "Black", grey: "Grey" };
  const variantNames = { pi5: "Raspberry Pi 5", byo: "BYO Compute", aura: "Luckfox Aura" };
  const priceElement = document.getElementById("product-price");
  const priceAmount = document.getElementById("price-amount");
  const selectedCaseElement = document.getElementById("selected-case");
  const caseNote = document.getElementById("case-note");
  const checkoutButton = document.getElementById("buy-button");
  const checkoutButtonLabel = document.getElementById("buy-button-label");
  const checkoutStatus = document.getElementById("checkout-status");
  const paymentNote = document.getElementById("payment-note");
  const configurationImage = document.getElementById("configuration-image");
  const configurationThumbnail = document.getElementById("configuration-thumbnail");
  const configurationCaption = document.getElementById("configuration-caption");
  const boxContent = document.getElementById("box-content");
  const buyPanel = document.getElementById("buy");
  const tabs = [...document.querySelectorAll("[data-show-slide]")];
  const slides = [...document.querySelectorAll("[data-slide]")];

  function stripeLinkFor(variant) {
    const raw = config.variants?.[variant]?.paymentLink;
    if (!raw || typeof raw !== "string") return null;
    try {
      const url = new URL(raw);
      if (url.protocol !== "https:" || url.hostname !== "buy.stripe.com") return null;
      return url.href;
    } catch {
      return null;
    }
  }

  function selectedVariant() {
    return document.querySelector('input[name="compute-variant"]:checked')?.value || "pi5";
  }

  function showSlide(index) {
    for (const slide of slides) {
      const active = Number(slide.dataset.slide) === index;
      slide.hidden = !active;
      slide.classList.toggle("is-active", active);
    }
    for (const tab of tabs) {
      const active = Number(tab.dataset.showSlide) === index;
      tab.classList.toggle("is-active", active);
      tab.setAttribute("aria-pressed", String(active));
    }
  }

  function updateConfiguration() {
    const chosenVariant = selectedVariant();
    const chosenColor = document.querySelector('input[name="case-color"]:checked')?.value || "black";
    const variant = Object.hasOwn(variantNames, chosenVariant) ? chosenVariant : "pi5";
    const color = Object.hasOwn(colorNames, chosenColor) ? chosenColor : "black";
    const price = config.variants?.[variant]?.price;
    const link = stripeLinkFor(variant);
    const picture = `./assets/configurations/${variant}-${color}.svg?v=4`;

    selectedCaseElement.textContent = colorNames[color];
    configurationImage.src = picture;
    configurationImage.alt = `Illustrative Nuvo Vision One ${variantNames[variant]} configuration with ${color} case`;
    configurationThumbnail.src = picture;
    configurationCaption.textContent = `${variantNames[variant]} · ${color} case · illustrative preview`;
    boxContent.textContent = variant === "pi5"
      ? "1 × Vision One stereo camera with Raspberry Pi 5 compute"
      : variant === "byo"
        ? "1 × Vision One stereo camera; compute board not included"
        : "Planned: 1 × Vision One stereo camera with Luckfox Aura compute";
    showSlide(0);

    if (Number.isFinite(price)) {
      priceAmount.textContent = new Intl.NumberFormat("en-US", { maximumFractionDigits: 0 }).format(price);
      priceElement.setAttribute("aria-label", `${price} US dollars`);
    }

    checkoutButton.disabled = !link;
    checkoutButtonLabel.textContent = link ? "Buy now" : "Checkout coming soon";
    checkoutButton.dataset.checkoutUrl = link || "";
    caseNote.textContent = link
      ? "Select your case color again in the checkout window; this preview does not carry over."
      : "This finish is a preview; select your final case color when checkout opens.";
    checkoutStatus.textContent = link
      ? "Continue to Stripe to choose your case color and complete checkout."
      : variant === "aura"
        ? "Luckfox Aura is coming soon; checkout is not yet available."
        : "BYO Compute checkout is not yet available.";
    paymentNote.textContent = link
      ? "Payment and shipping details are collected securely by Stripe."
      : "Online checkout will open when this configuration is available.";
  }

  document.querySelectorAll('input[name="compute-variant"], input[name="case-color"]').forEach(input => {
    input.addEventListener("change", updateConfiguration);
  });
  tabs.forEach(tab => tab.addEventListener("click", () => showSlide(Number(tab.dataset.showSlide))));
  document.querySelector('.header-cta[href="#buy"]').addEventListener("click", event => {
    event.preventDefault();
    buyPanel.scrollTop = 0;
    window.scrollTo({ top: 0, behavior: "smooth" });
  });
  checkoutButton.addEventListener("click", () => {
    const link = stripeLinkFor(selectedVariant());
    if (link) window.location.assign(link);
  });
  updateConfiguration();
})();
